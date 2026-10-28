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
MANIFEST = os.path.join(HERE, "explora_manifest.py")
BASE = "https://example.com/explora/v2"


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

    # ------------------------------------------------------------ manifest

    def signed_set(self):
        """Signed Pro gateway + HW3 sensor files (the test key)."""
        r, gw = self.sign(gateway_image(), "--board", "pro", name="g.bin", out="gw-pro-2.1.3.bin")
        self.assertEqual(r.returncode, 0, r.stderr)
        r, sn = self.sign(sensor_image(size=512), kind="sensor", name="s.bin", out="hw0003-0.2.12.bin")
        self.assertEqual(r.returncode, 0, r.stderr)
        return [gw, sn]

    def build(self, files, *extra, out="packages.json", seq="5"):
        dst = os.path.join(self.d, out)
        return run(MANIFEST, "build", "--base", BASE, "--seq", seq, "--key", self.key, *extra, "-o", dst,
                   *files), dst

    def mverify(self, path, *extra):
        return run(MANIFEST, "verify", "--pub", self.pub, *extra, path)

    def test_manifest_round_trip(self):
        files = self.signed_set()
        r, out = self.build(files)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("(verified)", r.stdout)
        data = read(out)
        head, body = data.split(b"\n", 1)
        self.assertRegex(head.decode(), r"^EXSM1 [0-9a-f]{8} [0-9a-f]{128}$")
        self.assertEqual(head.split()[1].decode(), "%08x" % sb.key_id(sb.load_public_key(self.pub)))
        import json
        doc = json.loads(body)
        self.assertEqual(doc["seq"], 5)
        self.assertEqual(list(doc["gateway"]), ["pro"])
        g = doc["gateway"]["pro"]
        self.assertEqual(g["version"], "2.1.3")
        self.assertEqual(g["url"], BASE + "/gw-pro-2.1.3.bin")
        self.assertEqual(g["size"], len(read(files[0])))
        import hashlib
        self.assertEqual(g["sha256"], hashlib.sha256(read(files[0])).hexdigest())
        self.assertEqual(doc["sensor"]["0003"]["version"], "0.2.12")
        v = self.mverify(out)
        self.assertEqual(v.returncode, 0, v.stdout)
        self.assertIn("valid manifest seq 5", v.stdout)
        # Replay floor
        self.assertEqual(self.mverify(out, "--min-seq", "6").returncode, 1)
        self.assertEqual(self.mverify(out, "--min-seq", "5").returncode, 0)
        # No key material printed
        self.assertNotIn("PRIVATE", r.stdout + r.stderr)
        # Existing output only with --force
        r, _ = self.build(files)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("exists", r.stderr)
        r, _ = self.build(files, "--force", seq="7")
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_manifest_tamper(self):
        r, out = self.build(self.signed_set())
        self.assertEqual(r.returncode, 0, r.stderr)
        good = read(out)
        for i in (150, len(good) // 2, len(good) - 2):
            bad = bytearray(good)
            bad[i] ^= 1
            v = self.mverify(self.file("t.json", bytes(bad)))
            self.assertEqual(v.returncode, 1, i)
        crlf = good.replace(b"\n", b"\r\n", 1)
        self.assertIn("line 1", self.mverify(self.file("c.json", crlf)).stdout)
        upper = good[:6] + good[6:14].upper() + good[14:]
        if upper != good:
            self.assertEqual(self.mverify(self.file("u.json", upper)).returncode, 1)
        zero = good[:6] + b"00000000" + good[14:]
        self.assertIn("key id 0", self.mverify(self.file("z.json", zero)).stdout)
        # Another key: not among the given ones
        other = os.path.join(self.keydir, "o.pem")
        self.assertEqual(run(KEYGEN, other).returncode, 0)
        v = run(MANIFEST, "verify", "--pub", os.path.join(self.keydir, "o.pub.pem"), out)
        self.assertEqual(v.returncode, 1)
        self.assertIn("not among the given keys", v.stdout)

    def test_manifest_refuses_unsigned_and_mismatched(self):
        files = self.signed_set()
        raw = self.file("hw0004.bin", sensor_image(hw=4))
        r, _ = self.build(files + [raw], out="a.json")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("unsigned", r.stderr)
        # Payload changed after signing
        bad = bytearray(read(files[1]))
        bad[40] ^= 1
        r, _ = self.build([files[0], self.file("hw0003-x.bin", bytes(bad))], out="b.json")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("SHA-256", r.stderr)
        # Two files for one HW type
        r, dup = self.sign(sensor_image(fw=0x00020D), kind="sensor", name="s2.bin", out="hw0003-0.2.13.bin")
        self.assertEqual(r.returncode, 0, r.stderr)
        r, _ = self.build(files + [dup], out="c.json")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("second sensor file", r.stderr)
        # Signed with a key that is neither the manifest key nor a --pub key
        other = os.path.join(self.keydir, "o.pem")
        self.assertEqual(run(KEYGEN, other).returncode, 0)
        src = self.file("g2.bin", gateway_image(chip=9))
        dst = os.path.join(self.d, "gw-lite.bin")
        self.assertEqual(run(SIGN, "sign", "--kind", "gateway", "--key", other, src, dst).returncode, 0)
        r, _ = self.build(files + [dst], out="d.json")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("not among the given keys", r.stderr)
        r, _ = self.build(files + [dst], "--pub", os.path.join(self.keydir, "o.pub.pem"), out="e.json")
        self.assertEqual(r.returncode, 0, r.stderr)
        # Bad base URL / key mode
        r = run(MANIFEST, "build", "--base", "http://example.com", "--seq", "1", "--key", self.key, "-o",
                os.path.join(self.d, "f.json"), *files)
        self.assertNotEqual(r.returncode, 0)
        os.chmod(self.key, 0o640)
        r, _ = self.build(files, out="g.json")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("chmod 600", r.stderr)
        os.chmod(self.key, 0o600)

    def test_manifest_seq_auto(self):
        files = self.signed_set()
        r, first = self.build(files, out="p1.json", seq="41")
        self.assertEqual(r.returncode, 0, r.stderr)
        r, second = self.build(files, "--prev", first, out="p2.json", seq="auto")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("seq 42", r.stdout)
        r, _ = self.build(files, out="p3.json", seq="auto")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("needs --prev", r.stderr)
        r, _ = self.build(files, "--prev", second, out="p4.json", seq="41")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("not above", r.stderr)
        # The same seq again: refused (two manifests under one seq)
        r, _ = self.build(files, "--prev", second, out="p5.json", seq="42")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("not above", r.stderr)
        r, _ = self.build(files, "--prev", second, out="p5.json", seq="43")
        self.assertEqual(r.returncode, 0, r.stderr)
        # A previous manifest that does not verify is refused
        bad = bytearray(read(second))
        bad[-3] ^= 1
        r, _ = self.build(files, "--prev", self.file("pbad.json", bytes(bad)), out="p6.json", seq="auto")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("--prev", r.stderr)
        # ... or one signed by another key
        other = os.path.join(self.keydir, "o.pem")
        self.assertEqual(run(KEYGEN, other).returncode, 0)
        r = run(MANIFEST, "build", "--base", BASE, "--seq", "100", "--key", other, "--pub", self.pub, "-o",
                os.path.join(self.d, "po.json"), *files)
        self.assertEqual(r.returncode, 0, r.stderr)
        r, _ = self.build(files, "--prev", os.path.join(self.d, "po.json"), out="p7.json", seq="auto")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("not among the given keys", r.stderr)
        # Only https:// URLs or files
        r, _ = self.build(files, "--prev", "http://example.com/packages.json", out="p8.json", seq="auto")
        self.assertNotEqual(r.returncode, 0)

    def test_manifest_size_limit(self):
        r, out = self.build(self.signed_set())
        self.assertEqual(r.returncode, 0, r.stderr)
        big = read(out) + b" " * (4097 - len(read(out)))
        v = self.mverify(self.file("big.json", big))
        self.assertEqual(v.returncode, 1)
        self.assertIn("more than 4096", v.stdout)
        import explora_manifest as em
        doc = {"seq": 1, "sensor": {"%04X" % i: {"version": "0.1.0", "url": "https://h/x", "size": 4096,
                                                  "sha256": "0" * 64} for i in range(1, 10)}}
        refusals, skips = em.check_json(doc)
        self.assertEqual(refusals, [])
        self.assertEqual(skips, ["sensor.0009 (more than 8 sensor packages)"])
        doc["sensor"] = {"00a3": doc["sensor"]["0001"]}
        self.assertEqual(em.check_json(doc)[1], ["sensor.00a3 (HW type key)"])

    def test_manifest_release_slot_limit(self):
        """Lite gateway files above the v1 slot (1984K) are refused by the release
        tool even though the gateway format allows 4 MB (layout v1 units remain)."""
        import explora_manifest as em
        self.assertEqual(em.RELEASE_GATEWAY_MAX["lite"], 0x1F0000)
        self.assertEqual(em.RELEASE_GATEWAY_MAX["pro"], 0x400000)
        limit = em.RELEASE_GATEWAY_MAX["lite"]
        r, ok = self.sign(gateway_image(chip=9, size=limit - 128), "--board", "lite", name="l1.bin", out="gw-lite-2.1.3.bin")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(len(read(ok)), limit)
        r, _ = self.build([ok], out="ok.json")
        self.assertEqual(r.returncode, 0, r.stderr)
        r, big = self.sign(gateway_image(chip=9, size=limit - 127), "--board", "lite", name="l2.bin", out="gw-lite-2.1.4.bin")
        self.assertEqual(r.returncode, 0, r.stderr)
        r, _ = self.build([big], out="big.json")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("release limit", r.stderr)
        # The same size is fine for the Pro
        r, pro = self.sign(gateway_image(size=limit - 127), "--board", "pro", name="p.bin", out="gw-pro-2.1.4.bin")
        self.assertEqual(r.returncode, 0, r.stderr)
        r, _ = self.build([pro], out="pro.json")
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_manifest_rules_mirror_the_gateway(self):
        """check_json as src/Update/Manifest.h (the native test_manifest
        corpus checks the firmware against the same function)."""
        import explora_manifest as em
        sha = "0" * 64

        def pk(**kw):
            p = {"version": "2.0.1", "url": "https://h.example/f.bin", "size": 100000, "sha256": sha}
            p.update(kw)
            return p

        def skips(p, kind="gateway"):
            doc = {"seq": 1, "gateway": {"pro": p}} if kind == "gateway" else {"seq": 1, "sensor": {"0003": p}}
            refusals, sk = em.check_json(doc)
            self.assertEqual(refusals, [])
            return sk

        # URL: bytes, host after the FIRST '@', 127-byte host, literals
        self.assertEqual(skips(pk(url="https://u:p@h.example:8443/f")), [])
        self.assertTrue(skips(pk(url="https://a@b@h.example/f")))
        self.assertTrue(skips(pk(url="https://u:p@ss@h.example/f")))
        self.assertEqual(skips(pk(url="https://" + "h" * 127 + "/f")), [])
        self.assertTrue(skips(pk(url="https://" + "h" * 128 + "/f")))
        self.assertEqual(skips(pk(url="https://h.example/" + "a" * 238)), [])
        self.assertTrue(skips(pk(url="https://h.example/" + "a" * 237 + "\u00e9")))  # 256 chars, 257 bytes
        self.assertEqual(skips(pk(url="https://[::1]/f")), [])
        self.assertTrue(skips(pk(url="https://h?x/f")))
        self.assertEqual(skips(pk(url="https://h:80?x/f")), [])  # the gateway checks the name before ':'
        self.assertTrue(skips(pk(url="https://:80/f")))
        self.assertTrue(skips(pk(url="https://h/a\x00b")))
        # Versions: leading zeros as the gateway, parts 0..255
        self.assertEqual(skips(pk(version="002.000.001")), [])
        self.assertTrue(skips(pk(version="2.0.256")))
        self.assertTrue(skips(pk(version="2.0.0-dev")))
        # Size limits per kind
        self.assertEqual(skips(pk(size=em.GATEWAY_MIN)), [])
        self.assertTrue(skips(pk(size=em.GATEWAY_MIN - 1)))
        self.assertTrue(skips(pk(size=em.GATEWAY_MAX + 1)))
        self.assertTrue(skips(pk(size=True)))
        self.assertEqual(skips(pk(size=em.SENSOR_MAX), "sensor"), [])
        self.assertTrue(skips(pk(size=em.SENSOR_MAX + 1), "sensor"))
        self.assertTrue(skips(pk(size=em.SENSOR_MIN - 1), "sensor"))
        # A null board is absent; unknown boards and fields are ignored
        self.assertEqual(em.check_json({"seq": 1, "gateway": {"pro": None, "c6": {"x": 1}}}), ([], []))
        # Structure: refused
        for bad in ({"seq": 1, "gateway": []}, {"seq": 1, "sensor": "x"}, {"seq": -1}, {"seq": 2 ** 32},
                    {"seq": True}, {"seq": 1.0}, {}, [1]):
            self.assertTrue(em.check_json(bad)[0], bad)
        # Node bound and nesting (ArduinoJson's limit 10, the root counts)
        self.assertEqual(em.check_json({"seq": 1, "x": list(range(em.MAX_NODES - 2))})[0], [])
        self.assertTrue(em.check_json({"seq": 1, "x": list(range(em.MAX_NODES - 1))})[0])
        deep = []
        for _ in range(8):
            deep = [deep]
        self.assertEqual(em.check_json({"seq": 1, "x": deep})[0], [])  # 10 levels
        self.assertTrue(em.check_json({"seq": 1, "x": [deep]})[0])  # 11
        # The JSON reader: trailing bytes ignored (as the gateway), stricter
        # than the gateway for duplicates / NaN / 01
        self.assertEqual(em.parse_body(b' {"seq":1} junk'), {"seq": 1})
        for bad in (b'{"seq":1,"seq":2}', b'{"seq":NaN}', b'{"seq":01}', b"{'seq':1}", b'{"seq":1,}'):
            with self.assertRaises(ValueError):
                em.parse_body(bad)

    def test_manifest_allow_skip(self):
        """A file the gateway would skip (here: a sensor image over this
        firmware's limit) is refused unless --allow-skip; verify likewise."""
        import explora_manifest as em
        files = self.signed_set()
        big = sensor_image(hw=0x10, size=em.SENSOR_MAX - sb.SIZE + 32)  # signed file 32 B over the limit
        r, f = self.sign(big, kind="sensor", name="big.bin", out="hw0010-0.2.12.bin")
        self.assertEqual(r.returncode, 0, r.stderr)
        r, _ = self.build(files + [f], out="s1.json")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("sensor.0010.size", r.stderr)
        self.assertIn("--allow-skip", r.stderr)
        r, out = self.build(files + [f], "--allow-skip", out="s2.json")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stderr.count("WARNING"), 1)
        v = self.mverify(out)
        self.assertEqual(v.returncode, 1)
        self.assertIn("entries the gateway skips", v.stdout)
        v = self.mverify(out, "--allow-skip")
        self.assertEqual(v.returncode, 0, v.stdout)
        self.assertIn("WARNING", v.stderr)
        # A forward-looking previous manifest still works as --prev (only
        # its signature and seq are used), with or without --allow-skip now
        r, _ = self.build(files, "--prev", out, out="s3.json", seq="auto")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("seq 6", r.stdout)
        self.assertNotIn("WARNING", r.stderr)
        r, _ = self.build(files, "--prev", out, out="s4.json", seq="5")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("not above", r.stderr)

    def test_prev_redirect_to_http_refused(self):
        import explora_manifest as em
        h = em.HttpsOnlyRedirects()
        import urllib.request
        req = urllib.request.Request("https://example.com/packages.json")
        with self.assertRaises(em.ManifestError):
            h.redirect_request(req, None, 302, "Found", {}, "http://example.com/packages.json")
        self.assertIsNotNone(h.redirect_request(req, None, 302, "Found", {}, "https://example.org/p.json"))

    def test_parse_version_ascii_only(self):
        self.assertEqual(sb.parse_version("2.0.0-dev"), 0x020000)
        for bad in ["１.2.3", "1.2.3\n", "١.2.3"]:
            with self.assertRaises(sb.FormatError):
                sb.parse_version(bad)


if __name__ == "__main__":
    unittest.main()
