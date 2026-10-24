#!/usr/bin/env python3
"""
Sign or verify an expLORA firmware file (gateway or sensor image).

  explora_sign.py sign --kind gateway|sensor [--key PEM] [--version X.Y.Z] in.bin out.bin
  explora_sign.py verify --pub PUBLIC.pem [--pub ...] file.bin

sign appends the 128-byte signature block (explora_sigblock.py). The
target and version come from the file itself: a sensor image's trailer
(HW type, fw version) or a gateway image's chip id (Pro / Lite) and
version (esp_app_desc_t.version when it is maj.min.pat, else the image's
EXPLORA_FW_VERSION marker). --version is only a cross-check against that;
an image whose version cannot be read is refused. A pre-release version
("2.0.0-dev") is refused unless --force.

The private key: --key PATH, else the EXPLORA_SIGN_KEY environment
variable (a path). It must not be readable by group or others. Keys are
never printed, only their key id. The written file is verified again with
the public key before the tool reports success.

verify checks the block against the file (kind, target, version, length,
SHA-256) and the signature with the given public key(s) (PEM public keys,
e.g. the .pub.pem written by explora_keygen.py). Exit status: 0 signed and
valid, 1 invalid, 2 unsigned.
"""

import argparse
import hashlib
import os
import stat
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import explora_sigblock as sb  # noqa: E402

BOARD_NAME = {sb.TARGET_PRO: "pro", sb.TARGET_LITE: "lite"}


def die(msg, code=1):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(code)


def key_path(arg):
    path = arg or os.environ.get("EXPLORA_SIGN_KEY")
    if not path:
        die("no key: pass --key or set EXPLORA_SIGN_KEY")
    return os.path.expanduser(path)


def check_key_mode(path):
    mode = os.stat(path).st_mode
    if mode & (stat.S_IRWXG | stat.S_IRWXO):
        die(f"{path} is accessible by group or others (mode {stat.S_IMODE(mode):03o}) - chmod 600 it")
    # Both the folder the path names and the folder of the real file: a
    # symlink's own folder decides who can swap the link, the real folder
    # who can replace the key; both owned by the user who signs
    folders = [os.path.dirname(os.path.abspath(path)), os.path.dirname(os.path.realpath(path))]
    for folder in dict.fromkeys(folders):
        fst = os.stat(folder)
        fmode = fst.st_mode
        if hasattr(os, "getuid") and fst.st_uid != os.getuid():
            die(f"the key folder {folder} belongs to uid {fst.st_uid}, not to you (uid {os.getuid()}) "
                "- keep the key in your own folder")
        if fmode & (stat.S_IWGRP | stat.S_IWOTH):
            die(f"the key folder {folder} is writable by group or others (mode {stat.S_IMODE(fmode):03o}) "
                "- the key could be replaced; chmod 700 it")


def describe(kind, target, version):
    if kind == sb.KIND_GATEWAY:
        return f"gateway {BOARD_NAME.get(target, target)} {sb.version_text(version)}"
    return f"sensor HW 0x{target:04X} {sb.version_text(version)}"


def check_file(info, payload, public_keys, version_override=None):
    """Problems (list of text) and notes of a parsed block against its payload."""
    problems, notes = [], []
    if info["length"] != len(payload):
        problems.append(f"payload length {len(payload)} B, block says {info['length']} B")
    if hashlib.sha256(payload).digest() != info["sha256"]:
        problems.append("payload SHA-256 differs from the block")
    try:
        if info["kind"] == sb.KIND_GATEWAY:
            target, version, text, source = sb.gateway_image_info(payload, version_override,
                                                                 require_version=False)
            if version is None:
                problems.append("the image carries no readable version (esp_app_desc_t or marker)")
        else:
            if version_override is not None:
                problems.append("--version applies to gateway images only")
            target, version = sb.sensor_image_info(payload)
        if target != info["target"]:
            problems.append(f"block target {info['target']}, image target {target}")
        if version is not None and version != info["version"]:
            problems.append(f"block version {sb.version_text(info['version'])}, "
                            f"image version {sb.version_text(version)}")
    except sb.FormatError as e:
        problems.append(f"payload: {e}")
    kid = info["key_id"]
    if kid not in public_keys:
        problems.append(f"key id {kid:08x} is not among the given keys")
    elif not sb.signature_ok(info, public_keys[kid]):
        problems.append("signature does not verify")
    return problems, notes


def write_exclusive(path, data, force):
    if os.path.lexists(path):
        if not force:
            die(f"{path} exists (use --force to replace it)")
    tmp = path + ".tmp"
    if os.path.lexists(tmp):
        if not force:
            die(f"{tmp} exists (a stale temporary file? use --force to remove it)")
        os.remove(tmp)
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0), 0o644)
    with os.fdopen(fd, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def cmd_sign(a):
    kind = sb.KINDS[a.kind]
    with open(a.input, "rb") as f:
        data = f.read()
    try:
        if kind == sb.KIND_SENSOR:
            if a.version is not None or a.board:
                die("--version / --board apply to gateway images only")
            target, version = sb.sensor_image_info(data)
            text = sb.version_text(version)
        else:
            target, version, text, source = sb.gateway_image_info(data, a.version)
            if a.board and sb.BOARDS[a.board] != target:
                die(f"the image is for the {BOARD_NAME[target]} board, not {a.board}")
            print(f"version {text} from the image's {source}")
        if sb.is_prerelease(text) and not a.force:
            die(f"version {text!r} is a pre-release (it would sign as {sb.version_text(version)}) - "
                "use --force to sign it anyway")
        path = key_path(a.key)
        check_key_mode(path)
        key = sb.load_private_key(path)
    except sb.FormatError as e:
        die(str(e))
    block = sb.build_block(kind, target, version, data, key)
    pub = key.public_key()
    write_exclusive(a.output, data + block, a.force)
    # Read back and verify what was written
    with open(a.output, "rb") as f:
        written = f.read()
    info = sb.parse_block(written[-sb.SIZE:])
    problems, _ = check_file(info, written[:-sb.SIZE], {sb.key_id(pub): pub}, a.version)
    if written[:-sb.SIZE] != data or problems:
        os.remove(a.output)
        die("the written file does not verify (" + "; ".join(problems or ["payload differs"]) + ") - removed")
    print(f"signed {describe(kind, target, version)}: {len(data)} B payload, "
          f"key id {sb.key_id(pub):08x} -> {a.output} (verified)")


def cmd_verify(a):
    with open(a.file, "rb") as f:
        data = f.read()
    if len(data) < sb.SIZE or data[-sb.SIZE:-sb.SIZE + 4] != sb.MAGIC:
        print("unsigned (no signature block)")
        return 2
    try:
        info = sb.parse_block(data[-sb.SIZE:])
    except sb.FormatError as e:
        print(f"invalid: {e}")
        return 1
    keys = {}
    for p in a.pub:
        try:
            k = sb.load_public_key(os.path.expanduser(p))
        except (sb.FormatError, OSError) as e:
            die(str(e))
        keys[sb.key_id(k)] = k
    print(f"block: {describe(info['kind'], info['target'], info['version'])}, "
          f"{info['length']} B payload, key id {info['key_id']:08x}")
    problems, notes = check_file(info, data[:-sb.SIZE], keys, a.version)
    for n in notes:
        print(f"note: {n}")
    if problems:
        for p in problems:
            print(f"invalid: {p}")
        return 1
    print("valid signature")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("sign", help="append a signature block")
    s.add_argument("--kind", choices=sorted(sb.KINDS), required=True)
    s.add_argument("--key", help="private key PEM (default: $EXPLORA_SIGN_KEY)")
    s.add_argument("--version", help="gateway only: expected version (checked against the image)")
    s.add_argument("--board", choices=sorted(sb.BOARDS), help="gateway only: expected board (checked)")
    s.add_argument("--force", action="store_true",
                   help="replace an existing output / stale .tmp, sign a pre-release version")
    s.add_argument("input")
    s.add_argument("output")
    v = sub.add_parser("verify", help="check a signed file")
    v.add_argument("--pub", action="append", required=True, help="public key PEM, repeatable")
    v.add_argument("--version", help="gateway only: expected version (checked against the image)")
    v.add_argument("file")
    a = ap.parse_args()
    if a.cmd == "sign":
        cmd_sign(a)
        return 0
    return cmd_verify(a)


if __name__ == "__main__":
    sys.exit(main())
