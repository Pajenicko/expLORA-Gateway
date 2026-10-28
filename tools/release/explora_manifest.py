#!/usr/bin/env python3
"""
Build or verify the signed package manifest (packages.json) the gateway
reads for its update check (src/Update/Manifest.h).

  explora_manifest.py build --base URL (--seq N | --seq auto --prev FILE_OR_URL)
                            [--key PEM] [--pub PUB.pem ...] [--allow-skip] [--force]
                            -o packages.json signed.bin ...
  explora_manifest.py verify --pub PUB.pem [--pub ...] [--min-seq N] [--allow-skip] FILE_OR_URL

build reads kind, target (board / HW type), version, size and SHA-256 of
every input from the file itself: each must be a signed file
(explora_sign.py) whose block matches its payload and verifies with the
manifest key's public key or one of --pub. Unsigned, mismatched or
duplicate files (two for one board / HW type) are refused. Each package
URL is --base + "/" + the file name (the files go next to packages.json).
--seq auto takes the seq of an existing manifest plus one (--prev: a
local file or an https:// URL, fetched read-only, redirects only to
https://); that manifest must verify with the key (or a --pub key) first.
With a number, --prev is optional and the number must be above its seq
(two different manifests under one seq would let a host pick either).

The private key: --key PATH, else $EXPLORA_SIGN_KEY - the same mode checks
as explora_sign.py, never printed. The output is written exclusively
(.tmp + rename; an existing file only with --force) and read back and
verified before the tool reports success.

Format: line 1 "EXSM1 <key id 8 hex> <r||s 128 hex>\\n" - ECDSA P-256 over
SHA-256 of the exact bytes after the first newline - then compact JSON:
{"seq":N,"issued":unix,"gateway":{"pro":{...},"lite":{...}},
 "sensor":{"0003":{"version","url","size","sha256"}}}.

The gateway's rules (src/Update/Manifest.h) are mirrored here exactly,
in two classes:
- the manifest is refused: more than 4096 bytes, line 1, the signature,
  not a JSON object, seq missing / not 0..2^32-1, "gateway" or "sensor"
  not an object, more than 384 JSON values or nesting deeper than 10
  (the gateway's bound is memory: at least 384 values always parse, a
  body with short strings may hold more - refusing above 384 here is the
  safe direction);
- an entry is skipped (never offered; this board's invalid entry means no
  gateway offer): a package whose version is not maj.min.pat (0..255), whose
  url is not https:// (<= 256 bytes, a host of <= 127 bytes, the host after
  the FIRST '@' of the authority, no '?', '#' or '@' left in it), whose size
  is outside the gateway's limits for its kind (gateway 153 B .. 4 MiB,
  sensor 512 .. 114816 B) or whose sha256 is not 64 lowercase hex digits;
  a sensor key that is not 4 uppercase hex digits (not 0000); a 9th valid
  sensor package and more.
build refuses to write a manifest with anything of either class, and
verify reports it as invalid. --allow-skip accepts skipped entries on
purpose (an entry for later firmware): each one is printed as a WARNING.
This tool is stricter than the gateway in a few points the gateway's
JSON reader tolerates (duplicate keys, NaN, numbers like 01, single
quotes, a NUL in a string, invalid UTF-8): refused here. Bytes after the
JSON value are ignored by both (they are signed anyway).

verify checks the signature line, the rules above and optionally the
replay floor (--min-seq). Exit status 0 valid, 1 invalid. No upload:
publishing is a separate step.
"""

import argparse
import hashlib
import json
import os
import re
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import explora_sigblock as sb  # noqa: E402
import explora_sign as es  # noqa: E402

from cryptography.exceptions import InvalidSignature  # noqa: E402
from cryptography.hazmat.primitives import hashes  # noqa: E402
from cryptography.hazmat.primitives.asymmetric import ec  # noqa: E402
from cryptography.hazmat.primitives.asymmetric.utils import (  # noqa: E402
    decode_dss_signature,
    encode_dss_signature,
)

MAX_FILE = 4096
MAX_SENSORS = 8
URL_MAX = 256          # bytes
HOST_MAX = 127         # bytes (the gateway's host buffer is 128)
MAX_NODES = 384        # Manifest::MAX_NODES: JSON values below the root
MAX_DEPTH = 10         # ArduinoJson's nesting limit (the root object counts)
# Size limits per kind, whole file (UpdateCheck.cpp: GatewayImage::HEADER_SIZE + 1
# + SigBlock::SIZE .. Manifest::GATEWAY_MAX; SensorFetch::MIN_FILE .. MAX_FILE)
GATEWAY_MIN = 24 + 1 + 128
GATEWAY_MAX = 0x400000
SENSOR_MIN = 512
SENSOR_MAX = 114688 + 128
HEADER_RE = re.compile(rb"EXSM1 ([0-9a-f]{8}) ([0-9a-f]{128})\n")
VERSION_RE = re.compile(r"([0-9]+)\.([0-9]+)\.([0-9]+)", re.ASCII)
HW_RE = re.compile(r"[0-9A-F]{4}", re.ASCII)
SHA_RE = re.compile(r"[0-9a-f]{64}", re.ASCII)
BOARD_KEY = {sb.TARGET_PRO: "pro", sb.TARGET_LITE: "lite"}
# Release rule (not a gateway rule - the gateway checks the size against its
# own slot): the largest gateway file a release may offer per board. The
# Lite stays at the 1984K slot of partition layout v1 while units with that
# layout may still exist: from firmware 2.0.1 on they migrate to layout v2
# (2752K slots) after the update, but a unit on 1.x / 2.0.0 that installs a
# larger file refuses it (larger than its slot). Raising "lite" to 0x2B0000:
# docs/release.md, "Lite partition layout".
RELEASE_GATEWAY_MAX = {"pro": 0x400000, "lite": 0x1F0000}


class ManifestError(Exception):
    pass


def die(msg, code=1):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(code)


# ---------------------------------------------------------------- rules (as the gateway)

def url_ok(url):
    """UpdateModePolicy::urlAccepted on the UTF-8 bytes: <= 256 bytes, none
    <= 0x20 or 0x7F, https://, the authority up to the first '/', the host
    after its FIRST '@', an IPv6 literal "[..." taken as it is, else the
    name up to the first ':' - 1..127 bytes without '?', '#' or '@'."""
    if not isinstance(url, str) or "\0" in url:
        return False
    try:
        b = url.encode("utf-8")
    except UnicodeEncodeError:
        return False
    if len(b) > URL_MAX or any(c <= 0x20 or c == 0x7F for c in b) or not b.startswith(b"https://"):
        return False
    authority = b[8:].split(b"/", 1)[0]
    host = authority.split(b"@", 1)[1] if b"@" in authority else authority
    if host.startswith(b"["):
        return True
    name = host.split(b":", 1)[0]
    return 0 < len(name) <= HOST_MAX and not any(c in b"?#@" for c in name)


def version_ok(text):
    """GatewayImage::parseVersion: maj.min.pat, digits only (leading zeros
    allowed), each part 0..255."""
    m = VERSION_RE.fullmatch(text) if isinstance(text, str) else None
    return bool(m) and all(int(x) <= 255 for x in m.groups())


def is_uint(v, top=0xFFFFFFFF):
    return isinstance(v, int) and not isinstance(v, bool) and 0 <= v <= top


def package_problem(p, lo, hi):
    """The first field of a package the gateway rejects (its order), or None."""
    if not isinstance(p, dict):
        return " (not an object)"
    if not version_ok(p.get("version")):
        return ".version"
    if not url_ok(p.get("url")):
        return ".url"
    if not is_uint(p.get("size")) or not lo <= p["size"] <= hi:
        return ".size"
    sha = p.get("sha256")
    if not isinstance(sha, str) or not SHA_RE.fullmatch(sha):
        return ".sha256"
    return None


def nodes_and_depth(v, depth=1):
    """(JSON values below v, the deepest container nesting with v at depth)."""
    if isinstance(v, dict):
        items = list(v.values())
    elif isinstance(v, list):
        items = v
    else:
        return 0, depth - 1
    n, deepest = len(items), depth
    for x in items:
        m, d = nodes_and_depth(x, depth + 1)
        n += m
        deepest = max(deepest, d)
    return n, deepest


def check_json(doc):
    """(refusals, skips) of a manifest JSON as the gateway reads it:
    refusals refuse the whole manifest; skips are entries a gateway does not
    use (the board named in "gateway.<board>" offers no update then)."""
    if not isinstance(doc, dict):
        return ["not a JSON object"], []
    nodes, depth = nodes_and_depth(doc)
    refusals, skips = [], []
    if nodes > MAX_NODES:
        refusals.append(f"{nodes} JSON values - more than {MAX_NODES} (too many entries)")
    if depth > MAX_DEPTH:
        refusals.append(f"nested {depth} deep - more than {MAX_DEPTH}")
    if not is_uint(doc.get("seq")):
        refusals.append("seq: missing or not an integer 0..2^32-1")
    gw, sensors = doc.get("gateway"), doc.get("sensor")
    if gw is not None and not isinstance(gw, dict):
        refusals.append("gateway: not an object")
    if sensors is not None and not isinstance(sensors, dict):
        refusals.append("sensor: not an object")
    if refusals:
        return refusals, skips
    for board in ("pro", "lite"):
        p = (gw or {}).get(board)
        if p is not None:
            why = package_problem(p, GATEWAY_MIN, GATEWAY_MAX)
            if why:
                skips.append(f"gateway.{board}{why} (the {board} board offers no gateway update)")
    kept = 0
    for k, p in (sensors or {}).items():
        if not HW_RE.fullmatch(k) or k == "0000":
            skips.append(f"sensor.{k} (HW type key)")
            continue
        why = package_problem(p, SENSOR_MIN, SENSOR_MAX)
        if why:
            skips.append(f"sensor.{k}{why}")
        elif kept >= MAX_SENSORS:
            skips.append(f"sensor.{k} (more than {MAX_SENSORS} sensor packages)")
        else:
            kept += 1
    return refusals, skips


def no_duplicates(pairs):
    keys = [k for k, _ in pairs]
    if len(set(keys)) != len(keys):
        raise ValueError("duplicate key " + repr(next(k for k in keys if keys.count(k) > 1)))
    return dict(pairs)


def no_constant(name):
    raise ValueError(f"{name} is not JSON the gateway reads")


def parse_body(body):
    """The JSON value at the start of body (after JSON whitespace); bytes
    after it are ignored, as the gateway does. ValueError if it is not
    JSON (or uses what the gateway reads differently)."""
    text = body.decode("utf-8")
    i = len(text) - len(text.lstrip(" \t\r\n"))
    dec = json.JSONDecoder(object_pairs_hook=no_duplicates, parse_constant=no_constant)
    doc, _ = dec.raw_decode(text, i)
    return doc


def split(data):
    """(key id, signature bytes, body) of a manifest file; ManifestError if
    line 1 is malformed."""
    if len(data) > MAX_FILE:
        raise ManifestError(f"{len(data)} bytes - more than {MAX_FILE}")
    m = HEADER_RE.match(data)
    if not m:
        raise ManifestError("line 1 is not 'EXSM1 <8 hex> <128 hex>\\n' (lowercase, LF only)")
    kid = int(m.group(1), 16)
    if kid == 0:
        raise ManifestError("key id 0")
    return kid, bytes.fromhex(m.group(2).decode()), data[m.end():]


def signature_ok(public_key, sig, body):
    der = encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big"))
    try:
        public_key.verify(der, body, ec.ECDSA(hashes.SHA256()))
        return True
    except InvalidSignature:
        return False


def verify_bytes(data, public_keys, min_seq=None, allow_skip=False, warn=True):
    """The parsed JSON of a valid manifest; ManifestError otherwise
    (skipped entries too, unless allow_skip: then they are printed as
    warnings)."""
    kid, sig, body = split(data)
    if kid not in public_keys:
        raise ManifestError(f"key id {kid:08x} is not among the given keys")
    if not signature_ok(public_keys[kid], sig, body):
        raise ManifestError("signature does not verify")
    try:
        doc = parse_body(body)
    except (UnicodeDecodeError, ValueError, RecursionError) as e:
        raise ManifestError(f"JSON: {e}")
    refusals, skips = check_json(doc)
    if refusals:
        raise ManifestError("; ".join(refusals))
    skips_allowed(skips, allow_skip, warn)
    if min_seq is not None and doc["seq"] < min_seq:
        raise ManifestError(f"seq {doc['seq']} is below {min_seq} (a gateway that saw {min_seq} refuses it)")
    return doc


def skips_allowed(skips, allow_skip, warn=True):
    """ManifestError for skipped entries, or (allow_skip) a WARNING each."""
    if skips and not allow_skip:
        raise ManifestError("entries the gateway skips: " + "; ".join(skips) +
                            " (--allow-skip only for entries meant for later firmware)")
    for s in skips if warn else []:
        print(f"WARNING: gateways running this firmware skip {s}", file=sys.stderr)


class HttpsOnlyRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not newurl.startswith("https://"):
            raise ManifestError(f"redirect to {newurl!r} refused (https:// only)")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def read_source(src):
    """A local file or an https:// URL (one read-only GET; redirects only
    to https://)."""
    if src.startswith("https://"):
        req = urllib.request.Request(src, headers={"User-Agent": "explora-manifest"})
        opener = urllib.request.build_opener(HttpsOnlyRedirects)
        try:
            with opener.open(req, timeout=20) as r:
                data = r.read(MAX_FILE + 1)
        except OSError as e:
            raise ManifestError(f"{src}: {e}")
        return data
    if "://" in src:
        raise ManifestError(f"{src}: only https:// URLs or local files")
    with open(src, "rb") as f:
        return f.read(MAX_FILE + 1)


def load_pubs(paths):
    keys = {}
    for p in paths or []:
        try:
            k = sb.load_public_key(os.path.expanduser(p))
        except (sb.FormatError, OSError) as e:
            die(str(e))
        keys[sb.key_id(k)] = k
    return keys


# ---------------------------------------------------------------- build

def package_of(path, public_keys):
    """(kind, target, entry) of a signed file; ManifestError when it is not
    a valid signed file."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < sb.SIZE or data[-sb.SIZE:-sb.SIZE + 4] != sb.MAGIC:
        raise ManifestError(f"{path}: unsigned (no signature block) - sign it with explora_sign.py first")
    try:
        info = sb.parse_block(data[-sb.SIZE:])
    except sb.FormatError as e:
        raise ManifestError(f"{path}: {e}")
    problems, _ = es.check_file(info, data[:-sb.SIZE], public_keys)
    if problems:
        raise ManifestError(f"{path}: " + "; ".join(problems))
    entry = {"version": sb.version_text(info["version"]), "url": None, "size": len(data),
             "sha256": hashlib.sha256(data).hexdigest()}
    return info["kind"], info["target"], entry


def body_of(doc):
    return (json.dumps(doc, separators=(",", ":")) + "\n").encode("ascii")


def sign_body(key, body):
    r, s = decode_dss_signature(key.sign(body, ec.ECDSA(hashes.SHA256())))
    return f"EXSM1 {sb.key_id(key.public_key()):08x} {r:064x}{s:064x}\n".encode("ascii") + body


def cmd_build(a):
    base = a.base.rstrip("/")
    if not url_ok(base + "/x"):
        die(f"--base {a.base!r} is not an https:// URL the gateway accepts")
    path = es.key_path(a.key)
    es.check_key_mode(path)
    try:
        key = sb.load_private_key(path)
    except sb.FormatError as e:
        die(str(e))
    pub = key.public_key()
    keys = {sb.key_id(pub): pub}
    keys.update(load_pubs(a.pub))

    prev_seq = None
    if a.prev:
        try:
            # Only its signature and seq matter: entries it carries that this
            # firmware skips (a forward-looking release built with
            # --allow-skip) are the published state, not refused here
            prev = verify_bytes(read_source(a.prev), keys, allow_skip=True, warn=False)
        except (ManifestError, OSError) as e:
            die(f"--prev: {e}")
        prev_seq = prev["seq"]
        print(f"previous manifest: seq {prev_seq} (signature verified)")
    if a.seq == "auto":
        if prev_seq is None:
            die("--seq auto needs --prev (the published manifest)")
        seq = prev_seq + 1
    else:
        if not re.fullmatch(r"[0-9]{1,10}", a.seq, re.ASCII):
            die("--seq must be a number or 'auto'")
        seq = int(a.seq)
        if prev_seq is not None and seq <= prev_seq:
            die(f"--seq {seq} is not above the previous manifest's {prev_seq} (gateways refuse a lower one; "
                "two manifests with one seq would let a host serve either)")
    if seq > 0xFFFFFFFF:
        die("seq above 2^32-1")

    gateway, sensor = {}, {}
    for f in a.files:
        try:
            kind, target, entry = package_of(f, keys)
        except (ManifestError, OSError) as e:
            die(str(e))
        entry["url"] = f"{base}/{os.path.basename(f)}"
        if kind == sb.KIND_GATEWAY:
            if target not in BOARD_KEY:
                die(f"{f}: unknown gateway board {target}")
            k = BOARD_KEY[target]
            if k in gateway:
                die(f"{f}: a second gateway file for the {k} board")
            if entry["size"] > RELEASE_GATEWAY_MAX[k]:
                die(f"{f}: {entry['size']} B - more than the {k} release limit {RELEASE_GATEWAY_MAX[k]} B "
                    "(the app slot units in the field still have)")
            gateway[k] = entry
        else:
            k = f"{target:04X}"
            if k == "0000" or len(k) != 4:
                die(f"{f}: HW type {target} cannot be a manifest key")
            if k in sensor:
                die(f"{f}: a second sensor file for HW type {k}")
            sensor[k] = entry
        print(f"  {f}: {es.describe(kind, target, sb.parse_version(entry['version']))}, "
              f"{entry['size']} B, sha256 {entry['sha256'][:16]}...")

    doc = {"seq": seq, "issued": int(time.time())}
    if gateway:
        doc["gateway"] = {k: gateway[k] for k in ("pro", "lite") if k in gateway}
    if sensor:
        doc["sensor"] = {k: sensor[k] for k in sorted(sensor)}
    refusals, skips = check_json(doc)
    if refusals:
        die("; ".join(refusals))
    try:
        skips_allowed(skips, a.allow_skip)
    except ManifestError as e:
        die(str(e))
    data = sign_body(key, body_of(doc))
    if len(data) > MAX_FILE:
        die(f"the manifest would be {len(data)} bytes - more than {MAX_FILE} (shorter --base?)")
    es.write_exclusive(a.output, data, a.force)
    with open(a.output, "rb") as f:
        written = f.read()
    try:
        verify_bytes(written, {sb.key_id(pub): pub}, allow_skip=a.allow_skip, warn=False)
    except ManifestError as e:
        os.remove(a.output)
        die(f"the written manifest does not verify ({e}) - removed")
    print(f"manifest seq {seq}, key id {sb.key_id(pub):08x}, {len(data)} B -> {a.output} (verified)")
    return 0


def cmd_verify(a):
    keys = load_pubs(a.pub)
    try:
        doc = verify_bytes(read_source(a.file), keys, a.min_seq, a.allow_skip)
    except (ManifestError, OSError) as e:
        print(f"invalid: {e}")
        return 1
    print(f"valid manifest seq {doc['seq']}")
    for board, p in sorted((doc.get("gateway") or {}).items()):
        if board in ("pro", "lite") and isinstance(p, dict):
            print(f"  gateway {board}: {p.get('version')} {p.get('size')} B {p.get('url')}")
    for hw, p in sorted((doc.get("sensor") or {}).items()):
        if isinstance(p, dict):
            print(f"  sensor {hw}: {p.get('version')} {p.get('size')} B {p.get('url')}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="write a signed manifest for signed .bin files")
    b.add_argument("--base", required=True, help="https:// folder the files are published in")
    b.add_argument("--seq", required=True, help="a number, or 'auto' (= --prev's seq + 1)")
    b.add_argument("--prev", help="the published manifest (file or https:// URL, read only)")
    b.add_argument("--key", help="private key PEM (default: $EXPLORA_SIGN_KEY)")
    b.add_argument("--pub", action="append", help="more public keys the files / --prev may be signed with")
    b.add_argument("--allow-skip", action="store_true",
                   help="accept entries this gateway firmware skips (meant for later firmware; warned)")
    b.add_argument("--force", action="store_true", help="replace an existing output / stale .tmp")
    b.add_argument("-o", "--output", required=True)
    b.add_argument("files", nargs="+")
    v = sub.add_parser("verify", help="check a manifest")
    v.add_argument("--pub", action="append", required=True, help="public key PEM, repeatable")
    v.add_argument("--min-seq", type=int, help="refuse a seq below this (replay floor)")
    v.add_argument("--allow-skip", action="store_true", help="skipped entries only warn")
    v.add_argument("file", help="file or https:// URL")
    a = ap.parse_args()
    return cmd_build(a) if a.cmd == "build" else cmd_verify(a)


if __name__ == "__main__":
    sys.exit(main())
