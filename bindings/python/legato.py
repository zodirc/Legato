#!/usr/bin/env python3
"""Load liblegato and check the C ABI version."""

import ctypes
import sys


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("usage: legato.py <liblegato.so>\n")
        return 2
    lib = ctypes.CDLL(argv[1])
    lib.legato_abi_version.restype = ctypes.c_uint32
    lib.legato_capabilities.restype = ctypes.c_char_p
    version = lib.legato_abi_version()
    caps = lib.legato_capabilities()
    if version != 1:
        sys.stderr.write("abi version %s\n" % version)
        return 1
    if caps is None or b'"skeleton":true' not in caps:
        sys.stderr.write("capabilities missing skeleton marker\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
