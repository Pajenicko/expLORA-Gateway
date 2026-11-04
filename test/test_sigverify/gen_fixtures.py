#!/usr/bin/env python3
"""
Generates fixtures.h for test_sigverify with the release tools
(tools/release): a synthetic Pro gateway image and a synthetic sensor image,
each signed through explora_sign.py with a throwaway TEST key, plus a block
signed by a second key that claims the test key's id.

Both private keys exist only in a temporary folder during the run and are
deleted afterwards; fixtures.h holds the public points and the signed files
only. Re-running creates new keys and new fixtures (the test does not
depend on particular key values).

  python3 test/test_sigverify/gen_fixtures.py
"""

import os
import struct
import subprocess
import sys
import tempfile
import zlib

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, "..", "..", "tools", "release")
sys.path.insert(0, TOOLS)
import explora_sigblock as sb  # noqa: E402


def gateway_image(version=b"2.1.3", chip=0, size=704):
    img = bytearray(size)
    img[0] = sb.ESP_IMAGE_MAGIC
    img[1] = 1  # segment count
    struct.pack_into("<H", img, 12, chip)
    struct.pack_into("<I", img, sb.APP_DESC_OFFSET, sb.ESP_APP_DESC_MAGIC)
    img[sb.APP_DESC_OFFSET + 16:sb.APP_DESC_OFFSET + 16 + len(version)] = version
    for i in range(sb.APP_DESC_OFFSET + 256, size):
        img[i] = (i * 7 + 3) & 0xFF
    return bytes(img)


def sensor_image(hw=0x0003, fw=(0 << 16 | 2 << 8 | 12), size=256):
    img = bytearray(size)
    for i in range(size - 32):
        img[i] = (i * 13 + 1) & 0xFF
    struct.pack_into("<I", img, sb.SENSOR_SIZE_WORD, size - 32)
    t = struct.pack("<IHHIIII", sb.SENSOR_TRAILER_MAGIC, 1, hw, fw, size - 32, sb.SENSOR_LOAD, 1 | 1 << 8)
    img[size - 32:size - 4] = t + bytes(28 - len(t))
    struct.pack_into("<I", img, size - 4, zlib.crc32(bytes(img[:size - 4])) & 0xFFFFFFFF)
    return bytes(img)


def write_key(path):
    """Private key at path (mode 600), public key at path + '.pub'."""
    key = ec.generate_private_key(ec.SECP256R1())
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption()))
    with open(path + ".pub", "wb") as f:
        f.write(sb.public_pem(key.public_key()))
    return key


# parse_version parity cases (C++ SigBlock::parseVersion must agree)
VERSION_TEXTS = [
    "2.0.0", "0.2.12", "255.255.255", "0.0.0", "001.002.003", "2.0.0-dev", "1.3.0+g12ab", "1.2.3-",
    "", "1", "1.2", "1.2.", "1.2.3.4", "256.0.0", "1.256.0", "1.2.256", "1000.0.0", "1.2.3x",
    "a.b.c", "1..3", "-1.2.3", " 1.2.3", "1.2.3 ", "6671d0b", "\uff11.2.3", "1.2.3\n", "+1.2.3",
]


def c_string(text):
    out = ""
    for b in text.encode("utf-8"):
        out += chr(b) if 32 <= b < 127 and chr(b) not in '"\\' else f"\\x{b:02x}\"\""
    return '"' + out + '"'


def version_cases():
    rows = []
    for t in VERSION_TEXTS:
        try:
            rows.append(f"    {{{c_string(t)}, true, 0x{sb.parse_version(t):06X}u}},")
        except sb.FormatError:
            rows.append(f"    {{{c_string(t)}, false, 0}},")
    return ("struct VersionCase\n{\n    const char *text;\n    bool ok;\n    uint32_t value;\n};\n"
            "static const VersionCase VERSION_CASES[] = {\n" + "\n".join(rows) + "\n};\n")


def c_array(name, data):
    rows = [", ".join(f"0x{b:02X}" for b in data[i:i + 16]) for i in range(0, len(data), 16)]
    return f"static const uint8_t {name}[{len(data)}] = {{\n    " + ",\n    ".join(rows) + "\n};\n"


def main():
    with tempfile.TemporaryDirectory() as tmp:
        ka, kb = os.path.join(tmp, "a.pem"), os.path.join(tmp, "b.pem")
        key_a, key_b = write_key(ka), write_key(kb)
        out = {}
        for name, kind, data in (("gw", "gateway", gateway_image()), ("sensor", "sensor", sensor_image())):
            src, dst = os.path.join(tmp, name + ".bin"), os.path.join(tmp, name + ".signed")
            with open(src, "wb") as f:
                f.write(data)
            subprocess.run([sys.executable, os.path.join(TOOLS, "explora_sign.py"), "sign", "--kind", kind,
                            "--key", ka, src, dst], check=True, stdout=subprocess.DEVNULL)
            r = subprocess.run([sys.executable, os.path.join(TOOLS, "explora_sign.py"), "verify",
                                "--pub", ka + ".pub", dst], stdout=subprocess.DEVNULL)
            assert r.returncode == 0, name
            with open(dst, "rb") as f:
                out[name] = f.read()
        gw = gateway_image()
        wrong = sb.build_block(sb.KIND_GATEWAY, sb.TARGET_PRO, sb.parse_version("2.1.3"), gw, key_b,
                               claimed_key_id=sb.key_id(key_a.public_key()))
        pa, pb = sb.public_point(key_a.public_key()), sb.public_point(key_b.public_key())
        ida, idb = sb.key_id(key_a.public_key()), sb.key_id(key_b.public_key())

    text = (
        "// Generated by gen_fixtures.py - do not edit. TEST keys only (private\n"
        "// parts were discarded); never a release key.\n#pragma once\n#include <cstdint>\n\n"
        f"static const uint32_t TEST_KEY_ID = 0x{ida:08X}u;\n"
        f"static const uint32_t OTHER_KEY_ID = 0x{idb:08X}u;\n"
        + c_array("TEST_KEY_POINT", pa) + c_array("OTHER_KEY_POINT", pb)
        + "// Pro gateway image 2.1.3 (synthetic), signed with the test key\n"
        + c_array("GW_SIGNED", out["gw"])
        + "// Sensor image HW 0x0003 0.2.12 (synthetic), signed with the test key\n"
        + c_array("SENSOR_SIGNED", out["sensor"])
        + "// Block for GW_SIGNED's payload signed by the other key, claiming TEST_KEY_ID\n"
        + c_array("GW_WRONG_KEY_BLOCK", wrong)
        + "// parse_version results of explora_sigblock.py\n" + version_cases()
    )
    with open(os.path.join(HERE, "fixtures.h"), "w") as f:
        f.write(text)
    print("fixtures.h written")


if __name__ == "__main__":
    main()
