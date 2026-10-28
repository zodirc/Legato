#!/usr/bin/env python3
"""Require a quiet-window report from quiet_tests."""

import re
import subprocess
import sys


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: expect_quiet.py <quiet_tests> <idle|pause>\n")
        return 2
    binary, mode = argv[1:]
    proc = subprocess.run([binary, mode], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    text = proc.stderr
    if proc.returncode != 0:
        sys.stderr.write(text)
        return 1
    wakes = re.search(r"quiet_wakes=(\d+)", text)
    if wakes is None or int(wakes.group(1)) != 0:
        sys.stderr.write(text)
        return 1
    if mode == "pause":
        hold = re.search(r"frame_hold failed=(\d+) checks=(\d+)", text)
        if hold is None or int(hold.group(1)) != 0 or int(hold.group(2)) < 1:
            sys.stderr.write(text)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
