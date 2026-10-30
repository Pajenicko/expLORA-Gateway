"""
expLORA signature block (shared by explora_sign.py, explora_keygen.py and
the test fixture generator). Same layout as src/Security/SigBlock.h:

  off   0  magic "EXSG"          4 B
        4  block version (1)     u16
        6  kind                  u16  1 = gateway, 2 = sensor
        8  target                u32  gateway board (1 Pro, 2 Lite) or sensor HW type
       12  firmware version      u32  maj << 16 | min << 8 | pat
       16  payload length        u32  bytes of the file before the block
       20  key id                u32  first 4 B of SHA-256(65-byte public point), big-endian value
       24  SHA-256(payload)      32 B
       56  signature r || s      64 B ECDSA P-256 over SHA-256(bytes 0..55)
      120  reserved              8 B  zero

All integers little-endian. Python 3 + cryptography.
"""

import hashlib
import re
import struct
import zlib

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import (
    decode_dss_signature,
    encode_dss_signature,
)

SIZE = 128
SIGNED_BYTES = 56
MAGIC = b"EXSG"
BLOCK_VERSION = 1
KIND_GATEWAY = 1
KIND_SENSOR = 2
KINDS = {"gateway": KIND_GATEWAY, "sensor": KIND_SENSOR}
TARGET_PRO = 1
TARGET_LITE = 2
BOARDS = {"pro": TARGET_PRO, "lite": TARGET_LITE}
# esp_image_header_t.chip_id -> gateway board
CHIP_BOARD = {0: TARGET_PRO, 9: TARGET_LITE}  # ESP32 -> Pro, ESP32-S3 -> Lite

_HEAD = struct.Struct("<4sHHIIII32s")  # bytes 0..55


class FormatError(Exception):
    pass


# ---------------------------------------------------------------- versions

def parse_version(text):
    """'maj.min.pat' (0..255 each), an optional '-'/'+' suffix is ignored."""
    m = re.fullmatch(r"([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})([-+].*)?", text, re.ASCII | re.DOTALL)
    if not m:
        raise FormatError(f"not a maj.min.pat version: {text!r}")
    parts = [int(x) for x in m.groups()[:3]]
    if any(p > 255 for p in parts):
        raise FormatError(f"version part above 255: {text!r}")
    return parts[0] << 16 | parts[1] << 8 | parts[2]


def is_prerelease(text):
    """True for a version text with a '-' or '+' suffix ("2.0.0-dev")."""
    return bool(re.fullmatch(r"[0-9.]+[-+].*", text, re.ASCII | re.DOTALL))


def version_text(v):
    return f"{v >> 16 & 0xFF}.{v >> 8 & 0xFF}.{v & 0xFF}"


# ---------------------------------------------------------------- keys

def public_point(public_key):
    return public_key.public_bytes(
        serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint
    )


def key_id(public_key):
    return struct.unpack(">I", hashlib.sha256(public_point(public_key)).digest()[:4])[0]


def load_private_key(path):
    with open(path, "rb") as f:
        data = f.read()
    try:
        key = serialization.load_pem_private_key(data, password=None)
    except (ValueError, TypeError) as e:
        raise FormatError(f"{path}: cannot load the private key ({e})")
    if not isinstance(key, ec.EllipticCurvePrivateKey) or key.curve.name != "secp256r1":
        raise FormatError(f"{path}: not an ECDSA P-256 private key")
    return key


def load_public_key(path):
    """A PEM public key (never a private key file)."""
    with open(path, "rb") as f:
        data = f.read()
    if b"PRIVATE KEY" in data:
        raise FormatError(f"{path}: a private key - pass the public key (.pub.pem)")
    try:
        key = serialization.load_pem_public_key(data)
    except ValueError as e:
        raise FormatError(f"{path}: not a PEM public key ({e})")
    if not isinstance(key, ec.EllipticCurvePublicKey) or key.curve.name != "secp256r1":
        raise FormatError(f"{path}: not an ECDSA P-256 key")
    return key


def public_pem(public_key):
    return public_key.public_bytes(serialization.Encoding.PEM,
                                   serialization.PublicFormat.SubjectPublicKeyInfo)


def c_table_entry(public_key, comment="release key"):
    """One ReleaseKeys::Key initializer for src/Security/ReleaseKeys.h."""
    point = public_point(public_key)
    rows = []
    for i in range(0, len(point), 13):
        rows.append("         " + ", ".join(f"0x{b:02X}" for b in point[i:i + 13]) + ",")
    return (f"    // {comment}, key id {key_id(public_key):08x}\n"
            f"    {{0x{key_id(public_key):08X}u,\n"
            "        {\n" + "\n".join(rows) + "\n        }},")


def release_key_table(path):
    """Filled slots of src/Security/ReleaseKeys.h as {key id: 65-byte point}.

    Parses the initializers c_table_entry() writes ({0xKEYIDu, {0x04, ...}});
    an empty slot ({0, {}}) is skipped. A file that is not UTF-8 text, has
    no KEYS table or SLOTS count, has more than one of either or any
    preprocessor conditional, has a slot in another form (the number of
    parsed slots must equal SLOTS), or a slot whose point is not 65 bytes /
    whose key id does not match its point is a FormatError (the caller must
    not guess)."""
    try:
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
    except UnicodeDecodeError:
        raise FormatError(f"{path}: not a UTF-8 text file")
    # one pass, so a // inside a block comment (or a /* after //) is harmless
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
    # the tool cannot tell which branch the compiler takes, nor which of two
    # tables the gateway uses: refuse instead of reading the first one
    if re.search(r"^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|elifdef|elifndef|else)\b", text, re.M):
        raise FormatError(f"{path}: preprocessor conditional in the key table header")
    if len(re.findall(r"\bSLOTS\s*=(?!=)", text)) > 1:
        raise FormatError(f"{path}: more than one SLOTS declaration")
    if len(re.findall(r"\bKEYS\s*\[[^\]]*\]\s*=(?!=)", text)) > 1:
        raise FormatError(f"{path}: more than one KEYS table")
    # the SLOTS declaration (a use such as KEYS[SLOTS] has no "=")
    n = re.search(r"\bSLOTS\s*=\s*(\d+)\s*;", text)
    if not n:
        raise FormatError(f"{path}: no SLOTS count")
    # the KEYS body up to its own "};" (slots end in "}}" / "},", never "};"),
    # so a later array's initializers are not counted as slots
    m = re.search(r"\bKEYS\s*\[[^\]]*\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        raise FormatError(f"{path}: no KEYS table")
    slots = re.findall(r"\{\s*(0x[0-9A-Fa-f]+|0)u?\s*,\s*\{([^{}]*)\}\s*\}", m.group(1))
    if not slots:
        raise FormatError(f"{path}: no slots in the KEYS table")
    if len(slots) != int(n.group(1)):
        raise FormatError(f"{path}: {len(slots)} of {n.group(1)} KEYS slots parsed - "
                          "write every slot as {0xKEYIDu, {0x04, ...}} or {0, {}}")
    table = {}
    for kid_text, body in slots:
        kid = int(kid_text, 16) if kid_text.lower().startswith("0x") else 0
        point = bytes(int(b, 16) for b in re.findall(r"0x([0-9A-Fa-f]{2})", body))
        if kid == 0:
            continue
        if len(point) != 65 or point[0] != 0x04:
            raise FormatError(f"{path}: slot {kid:08x} does not hold a 65-byte point")
        if struct.unpack(">I", hashlib.sha256(point).digest()[:4])[0] != kid:
            raise FormatError(f"{path}: slot {kid:08x} does not match its point")
        table[kid] = point
    return table


# ---------------------------------------------------------------- block

def build_block(kind, target, version, payload, private_key, claimed_key_id=None):
    """claimed_key_id: tests only - name another key than the signing one."""
    kid = key_id(private_key.public_key()) if claimed_key_id is None else claimed_key_id
    head = _HEAD.pack(MAGIC, BLOCK_VERSION, kind, target, version, len(payload), kid,
                      hashlib.sha256(payload).digest())
    der = private_key.sign(head, ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(der)
    block = head + r.to_bytes(32, "big") + s.to_bytes(32, "big") + bytes(8)
    assert len(block) == SIZE
    return block


def parse_block(block):
    if len(block) != SIZE:
        raise FormatError("block is not 128 bytes")
    magic, ver, kind, target, version, length, kid, digest = _HEAD.unpack(block[:SIGNED_BYTES])
    if magic != MAGIC:
        raise FormatError("no signature block (magic)")
    if ver != BLOCK_VERSION:
        raise FormatError(f"unknown block version {ver}")
    if kind not in (KIND_GATEWAY, KIND_SENSOR):
        raise FormatError(f"unknown kind {kind}")
    if any(block[120:]):
        raise FormatError("reserved bytes not zero")
    if kid == 0:
        raise FormatError("key id 0 (marks an empty key slot)")
    return {"kind": kind, "target": target, "version": version, "length": length,
            "key_id": kid, "sha256": digest, "r": block[56:88], "s": block[88:120],
            "head": block[:SIGNED_BYTES]}


def signature_ok(info, public_key):
    der = encode_dss_signature(int.from_bytes(info["r"], "big"), int.from_bytes(info["s"], "big"))
    try:
        public_key.verify(der, info["head"], ec.ECDSA(hashes.SHA256()))
        return True
    except InvalidSignature:
        return False


# ---------------------------------------------------------------- images

SENSOR_TRAILER_MAGIC = 0x544C5845  # "EXLT"
SENSOR_SIZE_WORD = 0x20
SENSOR_LOAD = 0x08002000


def sensor_image_info(data):
    """(hw type, fw version) of a trailered sensor image (protocol-v2.md 10.1),
    checked like the gateway's image store does (length, trailer, CRC)."""
    if len(data) < SENSOR_SIZE_WORD + 4 + 32:
        raise FormatError("file too short for a sensor image")
    size = struct.unpack_from("<I", data, SENSOR_SIZE_WORD)[0]
    if size + 32 != len(data):
        if size + 32 + SIZE == len(data) and data[size + 32:size + 36] == MAGIC:
            raise FormatError("the sensor image is already signed")
        raise FormatError("file length does not match the image size word")
    t = data[-32:]
    magic, tver, hw, fw, tsize, load, flags = struct.unpack_from("<IHHIIII", t)
    crc = struct.unpack_from("<I", t, 28)[0]
    if magic != SENSOR_TRAILER_MAGIC or tver != 1:
        raise FormatError("no image trailer v1")
    if tsize != size or load != SENSOR_LOAD:
        raise FormatError("trailer size or load address mismatch")
    if zlib.crc32(data[:-4]) & 0xFFFFFFFF != crc:
        raise FormatError("sensor image CRC mismatch")
    return hw, fw


# Trailer flags bits 4-5 (protocol-v2.md 10.1, sensor 1.0.1+): the identity the
# image needs - 0 not declared (built before 1.0.1), 1 any (bench build
# e77_bench), 2 a provisioned identity record (production build e77), 3 = 2
SENSOR_IDENTITY_TEXT = {0: "not declared (built before sensor 1.0.1)",
                        1: "any identity (bench build e77_bench)",
                        2: "provisioned identity record (production build e77)"}


def sensor_image_identity(data):
    """Identity class from the trailer flags of a checked sensor image"""
    flags = struct.unpack_from("<I", data, len(data) - 32 + 20)[0]
    return min((flags >> 4) & 3, 2)


# src/Security/VersionMarker.cpp - interim, until esp_app_desc_t carries
# FIRMWARE_VERSION (M4c S3)
VERSION_MARKER = b"EXPLORA_FW_VERSION="


def marker_version(data):
    """The version text of the image's single version marker, or None."""
    first = data.find(VERSION_MARKER)
    if first < 0:
        return None
    if data.find(VERSION_MARKER, first + 1) >= 0:
        raise FormatError("more than one version marker in the image")
    start = first + len(VERSION_MARKER)
    end = data.find(b"\0", start, start + 40)
    if end < 0:
        raise FormatError("unterminated version marker")
    return data[start:end].decode("ascii", "replace")


ESP_IMAGE_MAGIC = 0xE9
ESP_APP_DESC_MAGIC = 0xABCD5432
APP_DESC_OFFSET = 0x20  # image header 24 B + first segment header 8 B


def gateway_image_info(data, version_override=None, require_version=True):
    """(board, version, version text, source) of an ESP32 app image.

    The board comes from the image's chip id. The version is taken from
    esp_app_desc_t.version when that is maj.min.pat, else from the image's
    version marker (EXPLORA_FW_VERSION=...); when both are present they must
    be the same version. version_override (--version) is
    only a cross-check: it must equal the version found in the image, and an
    image with neither a descriptor version nor a marker is refused
    (version None when require_version is False).
    """
    if len(data) < APP_DESC_OFFSET + 48 + SIZE or data[0] != ESP_IMAGE_MAGIC:
        raise FormatError("not an ESP32 firmware image")
    if data[-SIZE:-SIZE + 4] == MAGIC:
        raise FormatError("the gateway image is already signed")
    chip = struct.unpack_from("<H", data, 12)[0]
    if chip not in CHIP_BOARD:
        raise FormatError(f"image for an unknown chip (id {chip})")
    magic = struct.unpack_from("<I", data, APP_DESC_OFFSET)[0]
    if magic != ESP_APP_DESC_MAGIC:
        raise FormatError("no esp_app_desc_t in the image")
    raw = data[APP_DESC_OFFSET + 16:APP_DESC_OFFSET + 48].split(b"\0")[0].decode("ascii", "replace")
    marker = marker_version(data)
    try:
        text, source = raw, "esp_app_desc_t"
        version = parse_version(raw)
    except FormatError:
        text, source = marker, "version marker"
        if text is None:
            if require_version:
                raise FormatError(f"esp_app_desc_t.version is {raw!r} and the image has no "
                                  "EXPLORA_FW_VERSION marker - its version is unknown")
            return CHIP_BOARD[chip], None, None, None
        version = parse_version(text)
    else:
        if marker is not None and marker != raw:
            raise FormatError(f"esp_app_desc_t.version {raw!r} differs from the image's "
                              f"version marker {marker!r}")
    if version_override is not None and parse_version(version_override) != version:
        raise FormatError(f"--version {version_override} differs from the image's {source} {text!r}")
    return CHIP_BOARD[chip], version, text, source
