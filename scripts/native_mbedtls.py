"""
Host mbedtls for the native tests (env:native only).

src/Security/SigVerify.cpp calls mbedtls, which the ESP32 firmware gets
from ESP-IDF. For the native tests the same mbedtls version (3.6.6) is
built from the official release archive: downloaded once into
.pio/native-deps (checked against the published SHA-256), compiled with
the minimal configuration test/native_mbedtls/mbedtls_config_native.h and
linked as a static library into every native test program (objects are
pulled in only by the tests that use them). The first run needs network
access, like the other library dependencies.
"""

import hashlib
import os
import shutil
import tarfile
import tempfile
import urllib.request

Import("env")  # noqa: F821

VERSION = "3.6.6"
URL = (
    "https://github.com/Mbed-TLS/mbedtls/releases/download/"
    f"mbedtls-{VERSION}/mbedtls-{VERSION}.tar.bz2"
)
SHA256 = "8fb65fae8dcae5840f793c0a334860a411f884cc537ea290ce1c52bb64ca007a"

project = env.subst("$PROJECT_DIR")  # noqa: F821
deps = os.path.join(env.subst("$PROJECT_WORKSPACE_DIR"), "native-deps")  # noqa: F821
root = os.path.join(deps, f"mbedtls-{VERSION}")
marker = os.path.join(root, ".explora-ok")


def fetch():
    os.makedirs(deps, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=deps) as tmp:
        archive = os.path.join(tmp, "mbedtls.tar.bz2")
        print(f"native_mbedtls: downloading mbedtls {VERSION}")
        with urllib.request.urlopen(URL, timeout=120) as r, open(archive, "wb") as f:
            shutil.copyfileobj(r, f)
        h = hashlib.sha256()
        with open(archive, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 16), b""):
                h.update(chunk)
        if h.hexdigest() != SHA256:
            raise SystemExit(f"native_mbedtls: SHA-256 mismatch for {URL}")
        out = os.path.join(tmp, "x")
        if not hasattr(tarfile, "data_filter"):
            raise SystemExit(
                "native_mbedtls: this Python has no tarfile extraction filters "
                "(needs 3.12+, or 3.8-3.11 with the 2023 security update) - "
                "refusing to unpack the archive without them"
            )
        with tarfile.open(archive, "r:bz2") as t:
            t.extractall(out, filter="data")
        top = os.path.join(out, f"mbedtls-{VERSION}")
        if not os.path.isdir(os.path.join(top, "library")):
            raise SystemExit("native_mbedtls: unexpected archive layout")
        shutil.rmtree(root, ignore_errors=True)
        shutil.move(top, root)
    open(marker, "w").close()


if not os.path.exists(marker):
    fetch()

config_dir = os.path.join(project, "test", "native_mbedtls")
env.Append(  # noqa: F821
    CPPPATH=[os.path.join(root, "include"), config_dir],
    CPPDEFINES=[("MBEDTLS_CONFIG_FILE", '\\"mbedtls_config_native.h\\"')],
)
lib_env = env.Clone()  # noqa: F821
lib_env.Replace(CCFLAGS=["-O2"], CFLAGS=["-std=c99"])
lib = lib_env.BuildLibrary(
    os.path.join("$BUILD_DIR", "native-mbedtls"),
    os.path.join(root, "library"),
    src_filter="+<*.c>",
)
env.Prepend(LIBS=[lib])  # noqa: F821
