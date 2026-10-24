#!/usr/bin/env python3
"""
Tests of the release tools (keygen, sign, verify) with throwaway keys in a
temporary folder. Run: python3 tools/release/test_release_tools.py
"""

import os
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import explora_sigblock as sb  # noqa: E402

SIGN = os.path.join(HERE, "explora_sign.py")
KEYGEN = os.path.join(HERE, "explora_keygen.py")


def gateway_image(desc_version=b"2.1.3", marker=None, chip=0, size=1024):
    img = bytearray(size)
    img[0] = sb.ESP_IMAGE_MAGIC
    struct.pack_into("<H", img, 12, chip)
    struct.pack_into("<I", img, sb.APP_DESC_OFFSET, sb.ESP_APP_DESC_MAGIC)
    img[sb.APP_DESC_OFFSET + 16:sb.APP_DESC_OFFSET + 16 + len(desc_version)] = desc_version
    for i in range(300, size):
        img[i] = (i * 7 + 3) & 0xFF
    if marker is not None:
        m = sb.VERSION_MARKER + marker + b"\0"
        img[400:400 + len(m)] = m
    return bytes(img)


def sensor_image(hw=3, fw=0x00020C, size=256):
    img = bytearray((i * 13 + 1) & 0xFF for i in range(size))
    struct.pack_into("<I", img, sb.SENSOR_SIZE_WORD, size - 32)
    t = struct.pack("<IHHIIII", sb.SENSOR_TRAILER_MAGIC, 1, hw, fw, size - 32, sb.SENSOR_LOAD, 0x101)
    img[size - 32:size - 4] = t + bytes(28 - len(t))
    struct.pack_into("<I", img, size - 4, zlib.crc32(bytes(img[:size - 4])))
    return bytes(img)


def read(path):
    with open(path, "rb") as f:
        return f.read()


def run(*args, env=None):
    e = dict(os.environ)
    e.pop("EXPLORA_SIGN_KEY", None)
    e.update(env or {})
    return subprocess.run([sys.executable] + list(args), capture_output=True, text=True, env=e)


class Tools(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = self.tmp.name
        self.keydir = os.path.join(self.d, "keys")
        self.key = os.path.join(self.keydir, "k.pem")
        self.pub = os.path.join(self.keydir, "k.pub.pem")
        r = run(KEYGEN, self.key)
        self.assertEqual(r.returncode, 0, r.stderr)

    def tearDown(self):
        self.tmp.cleanup()

    def file(self, name, data):
        p = os.path.join(self.d, name)
        with open(p, "wb") as f:
            f.write(data)
        return p

    def sign(self, data, *extra, kind="gateway", name="in.bin", out="out.bin"):
        src = self.file(name, data)
        dst = os.path.join(self.d, out)
        return run(SIGN, "sign", "--kind", kind, "--key", self.key, *extra, src, dst), dst

    def verify(self, path, *extra):
        return run(SIGN, "verify", "--pub", self.pub, *extra, path)

    # ------------------------------------------------------------ keygen

    def test_keygen_files_and_modes(self):
        self.assertEqual(stat.S_IMODE(os.stat(self.key).st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(os.stat(self.keydir).st_mode), 0o700)
        self.assertTrue(os.path.exists(self.pub))
        with open(self.pub, "rb") as f:
            self.assertNotIn(b"PRIVATE", f.read())
        r = run(KEYGEN, self.key)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("refusing to overwrite", r.stderr)
        r = run(KEYGEN, "--show", self.key)
        self.assertEqual(r.returncode, 0)
        self.assertNotIn("PRIVATE", r.stdout)

    def test_keygen_tightens_open_folder(self):
        folder = os.path.join(self.d, "open")
        os.mkdir(folder, 0o755)
        os.chmod(folder, 0o755)
        r = run(KEYGEN, os.path.join(folder, "x.pem"))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("tightened to 700", r.stderr)
        self.assertEqual(stat.S_IMODE(os.stat(folder).st_mode), 0o700)

    def test_keygen_pub_write_failure_removes_private_key(self):
        import contextlib
        import io
        import explora_keygen as kg
        real = kg.write_new
        calls = []

        def failing(path, data, mode):
            calls.append(path)
            if path.endswith(".pub.pem"):
                raise OSError(28, "No space left on device")
            return real(path, data, mode)

        path = os.path.join(self.keydir, "f.pem")
        err = io.StringIO()
        old_argv = sys.argv
        kg.write_new = failing
        sys.argv = ["explora_keygen.py", path]
        try:
            with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
                rc = kg.main()
        finally:
            kg.write_new = real
            sys.argv = old_argv
        self.assertEqual(rc, 1)
        self.assertEqual(len(calls), 2)
        self.assertFalse(os.path.lexists(path))
        self.assertFalse(os.path.lexists(kg.pub_path(path)))
        self.assertIn("private key was removed", err.getvalue())
        self.assertNotIn("Traceback", err.getvalue())

    def test_keygen_interrupt_removes_private_key(self):
        import contextlib
        import io
        import explora_keygen as kg
        real = kg.write_new

        def interrupted(path, data, mode):
            if path.endswith(".pub.pem"):
                raise KeyboardInterrupt()
            return real(path, data, mode)

        path = os.path.join(self.keydir, "i.pem")
        err = io.StringIO()
        old_argv = sys.argv
        kg.write_new = interrupted
        sys.argv = ["explora_keygen.py", path]
        try:
            with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(KeyboardInterrupt):
                    kg.main()
        finally:
            kg.write_new = real
            sys.argv = old_argv
        self.assertFalse(os.path.lexists(path))
        self.assertIn("private key was removed", err.getvalue())

    def test_keygen_mode_despite_umask(self):
        import explora_keygen as kg
        old = os.umask(0o777)
        try:
            p = os.path.join(self.keydir, "u.bin")
            kg.write_new(p, b"x", 0o600)
        finally:
            os.umask(old)
        self.assertEqual(stat.S_IMODE(os.stat(p).st_mode), 0o600)

    # ------------------------------------------------------------ sign

    def test_gateway_sign_verify(self):
        r, out = self.sign(gateway_image(), "--board", "pro")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("(verified)", r.stdout)
        self.assertEqual(self.verify(out).returncode, 0)

    def test_sensor_sign_verify(self):
        r, out = self.sign(sensor_image(), kind="sensor")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.verify(out).returncode, 0)

    def test_version_from_marker_and_cross_check(self):
        img = gateway_image(desc_version=b"6671d0b", marker=b"2.0.0")
        r, out = self.sign(img)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("version marker", r.stdout)
        self.assertEqual(sb.parse_block(read(out)[-128:])["version"], 0x020000)
        r, _ = self.sign(img, "--version", "2.0.1", out="o2.bin")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("differs", r.stderr)
        r, _ = self.sign(img, "--version", "2.0.0", out="o3.bin")
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_unknown_version_refused(self):
        r, _ = self.sign(gateway_image(desc_version=b"6671d0b"), "--version", "2.0.0")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("version is unknown", r.stderr)
        r, _ = self.sign(gateway_image(desc_version=b"2.1.3"), "--version", "2.1.4")
        self.assertNotEqual(r.returncode, 0)

    def test_descriptor_and_marker_must_agree(self):
        r, _ = self.sign(gateway_image(desc_version=b"2.0.0", marker=b"2.0.1"))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("differs from the image's version marker", r.stderr)
        r, out = self.sign(gateway_image(desc_version=b"2.0.0", marker=b"2.0.0"), out="same.bin")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("esp_app_desc_t", r.stdout)

    def test_two_markers_refused(self):
        img = bytearray(gateway_image(desc_version=b"x", marker=b"2.0.0"))
        m = sb.VERSION_MARKER + b"2.0.1\0"
        img[600:600 + len(m)] = m
        r, _ = self.sign(bytes(img))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("more than one", r.stderr)

    def test_prerelease_needs_force(self):
        img = gateway_image(desc_version=b"x", marker=b"2.0.0-dev")
        r, _ = self.sign(img)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("pre-release", r.stderr)
        r, _ = self.sign(img, "--force")
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_key_mode_checked(self):
        os.chmod(self.key, 0o640)
        r, _ = self.sign(gateway_image())
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("chmod 600", r.stderr)

    def test_key_folder_mode_checked(self):
        os.chmod(self.keydir, 0o770)
        r, _ = self.sign(gateway_image())
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("writable by group or others", r.stderr)
        os.chmod(self.keydir, 0o700)
        r, _ = self.sign(gateway_image(), out="ok.bin")
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_key_folder_of_symlink_target_checked(self):
        # A link in a private folder to a key in a private folder is fine ...
        open_dir = os.path.join(self.d, "open")
        os.mkdir(open_dir)
        os.chmod(open_dir, 0o700)
        link = os.path.join(open_dir, "k.pem")
        os.symlink(self.key, link)
        src = self.file("in.bin", gateway_image())
        r = run(SIGN, "sign", "--kind", "gateway", "--key", link, src, os.path.join(self.d, "a.bin"))
        self.assertEqual(r.returncode, 0, r.stderr)
        # ... a link in an open folder is not (anyone can point it elsewhere) ...
        os.chmod(open_dir, 0o777)
        r = run(SIGN, "sign", "--kind", "gateway", "--key", link, src, os.path.join(self.d, "a2.bin"))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("writable by group or others", r.stderr)
        self.assertIn(open_dir, r.stderr)
        os.remove(link)
        # ... nor a link in the private folder to a key in an open folder
        os.chmod(open_dir, 0o700)
        real = os.path.join(open_dir, "real.pem")
        os.rename(self.key, real)
        os.symlink(real, self.key)
        os.chmod(open_dir, 0o777)
        r, _ = self.sign(gateway_image(), out="b.bin")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("writable by group or others", r.stderr)

    @unittest.skipUnless(hasattr(os, "getuid") and os.getuid() == 0, "needs root to hand the folder to another uid")
    def test_key_folder_owner_checked(self):
        os.chown(self.keydir, 65534, -1)
        try:
            r, _ = self.sign(gateway_image())
        finally:
            os.chown(self.keydir, 0, -1)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("not to you", r.stderr)

    def test_output_and_stale_tmp(self):
        r, out = self.sign(gateway_image())
        self.assertEqual(r.returncode, 0)
        r, _ = self.sign(gateway_image())
        self.assertIn("exists", r.stderr)
        self.file("new.bin.tmp", b"stale")
        r, _ = self.sign(gateway_image(), out="new.bin")
        self.assertIn("stale", r.stderr)
        r, _ = self.sign(gateway_image(), "--force", out="new.bin")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse(os.path.exists(os.path.join(self.d, "new.bin.tmp")))

    def test_already_signed_refused(self):
        r, out = self.sign(gateway_image())
        r2 = run(SIGN, "sign", "--kind", "gateway", "--key", self.key, out, out + "2")
        self.assertNotEqual(r2.returncode, 0)
        self.assertIn("already signed", r2.stderr)

    # ------------------------------------------------------------ verify

    def test_verify_needs_public_key(self):
        r, out = self.sign(gateway_image())
        self.assertNotEqual(run(SIGN, "verify", out).returncode, 0)  # --pub is required
        r = run(SIGN, "verify", "--pub", self.key, out)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("private key", r.stderr)

    def test_verify_unsigned(self):
        self.assertEqual(self.verify(self.file("u.bin", gateway_image())).returncode, 2)

    def test_verify_tampered(self):
        r, out = self.sign(gateway_image())
        good = read(out)
        n = len(good) - 128
        cases = {"payload": 700, "kind": n + 6, "target": n + 8, "version": n + 12, "length": n + 16,
                 "key id": n + 20, "hash": n + 30, "r": n + 60, "s": n + 100, "reserved": n + 125}
        for name, off in cases.items():
            bad = bytearray(good)
            bad[off] ^= 0x01
            r = self.verify(self.file("t.bin", bytes(bad)))
            self.assertEqual(r.returncode, 1, f"{name}: {r.stdout}")
        # Another key
        other = os.path.join(self.d, "other", "o.pem")
        run(KEYGEN, other)
        r = run(SIGN, "verify", "--pub", other[:-4] + ".pub.pem", out)
        self.assertEqual(r.returncode, 1)
        self.assertIn("not among the given keys", r.stdout)
        # Truncated by one byte: no block at the end any more
        self.assertEqual(self.verify(self.file("c.bin", good[:-1])).returncode, 2)

    def test_key_id_zero_is_malformed(self):
        key = sb.load_private_key(self.key)
        block = sb.build_block(sb.KIND_SENSOR, 3, 0x00020C, sensor_image(), key, claimed_key_id=0)
        with self.assertRaises(sb.FormatError):
            sb.parse_block(block)
        good = sb.build_block(sb.KIND_SENSOR, 3, 0x00020C, sensor_image(), key)
        self.assertEqual(sb.parse_block(good)["key_id"], sb.key_id(key.public_key()))

    def test_parse_version_ascii_only(self):
        self.assertEqual(sb.parse_version("2.0.0-dev"), 0x020000)
        for bad in ["１.2.3", "1.2.3\n", "١.2.3"]:
            with self.assertRaises(sb.FormatError):
                sb.parse_version(bad)


if __name__ == "__main__":
    unittest.main()
