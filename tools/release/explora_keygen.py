#!/usr/bin/env python3
"""
Create the ECDSA P-256 release key pair (owner only).

  explora_keygen.py PATH            PATH of the new private key, outside any repository

Writes the private key as unencrypted PKCS#8 PEM with mode 600 and the
public key next to it (PATH with .pub.pem, mode 644, for
explora_sign.py verify). The key folder must be private: it is created
with mode 700, an existing folder of ours that group or others can access
is tightened to 700, one owned by someone else is refused; a folder above
it that others can write to is reported. An existing key file is never
overwritten; both files are created exclusively, and when the public key
cannot be written the private key is removed again. Keep the key outside every repository, with an offline
backup. Prints the key id and the public slot for
src/Security/ReleaseKeys.h - never the private key.

  explora_keygen.py --show PATH     print the slot of an existing key again
"""

import argparse
import os
import stat
import sys

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import explora_sigblock as sb  # noqa: E402


def pub_path(path):
    return (path[:-4] if path.endswith(".pem") else path) + ".pub.pem"


def private_folder(folder):
    if not os.path.isdir(folder):
        os.makedirs(folder, mode=0o700)
        os.chmod(folder, 0o700)
    st = os.stat(folder)
    if st.st_uid != os.getuid():
        raise SystemExit(f"error: {folder} belongs to another user - refusing to put a key there")
    if st.st_mode & (stat.S_IRWXG | stat.S_IRWXO):
        print(f"warning: {folder} was mode {stat.S_IMODE(st.st_mode):03o} - tightened to 700", file=sys.stderr)
        os.chmod(folder, 0o700)
    parent = os.path.dirname(folder)
    while parent and parent != os.path.dirname(parent):
        mode = os.stat(parent).st_mode
        if mode & (stat.S_IWGRP | stat.S_IWOTH) and not mode & stat.S_ISVTX:
            print(f"warning: {parent} is writable by group or others - the key folder could be "
                  "replaced; check it", file=sys.stderr)
        parent = os.path.dirname(parent)


def write_new(path, data, mode):
    """Create path exclusively (never through a symlink) and write data; a
    partly written file is removed again."""
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0), mode)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fchmod(f.fileno(), mode)  # the umask may have cut the mode at creation
            os.fsync(f.fileno())
    except BaseException:
        try:
            os.unlink(path)
        except OSError:
            pass
        raise


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path")
    ap.add_argument("--show", action="store_true", help="print the slot of an existing key")
    a = ap.parse_args()
    path = os.path.abspath(os.path.expanduser(a.path))
    if a.show:
        try:
            pub = sb.load_private_key(path).public_key()
        except (sb.FormatError, OSError) as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
    else:
        try:
            private_folder(os.path.dirname(path))
        except OSError as e:
            print(f"error: key folder {os.path.dirname(path)}: {e}", file=sys.stderr)
            return 1
        if os.path.lexists(path) or os.path.lexists(pub_path(path)):
            print(f"error: {path} or its .pub.pem exists - refusing to overwrite a key", file=sys.stderr)
            return 1
        while True:  # key id 0 marks an empty slot in ReleaseKeys.h
            key = ec.generate_private_key(ec.SECP256R1())
            if sb.key_id(key.public_key()) != 0:
                break
        pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                serialization.NoEncryption())
        pub = key.public_key()
        try:
            write_new(path, pem, 0o600)
        except OSError as e:
            print(f"error: cannot write {path}: {e}", file=sys.stderr)
            return 1
        try:
            write_new(pub_path(path), sb.public_pem(pub), 0o644)
        except BaseException as e:
            # Never leave a private key without its public half (verify needs
            # it), whatever ended the second write (also Ctrl-C)
            try:
                os.unlink(path)
                gone = "the private key was removed again"
            except OSError as e2:
                gone = f"could not remove the private key {path} ({e2}) - delete it by hand"
            if not isinstance(e, OSError):
                print(f"error: interrupted while writing {pub_path(path)}; {gone}", file=sys.stderr)
                raise
            print(f"error: cannot write {pub_path(path)}: {e}; {gone}", file=sys.stderr)
            return 1
        print(f"private key written to {path} (mode 600) - back it up offline, never commit it")
        print(f"public key written to {pub_path(path)}")
    print(f"key id {sb.key_id(pub):08x}; slot for src/Security/ReleaseKeys.h:")
    print(sb.c_table_entry(pub))
    return 0


if __name__ == "__main__":
    sys.exit(main())
