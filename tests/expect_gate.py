#!/usr/bin/env python3
"""Run one T1 scenario and require the named gate to fail."""

import re
import subprocess
import sys


def main(argv):
    if len(argv) != 5:
        sys.stderr.write("usage: expect_gate.py <legato-bench> <scenario> <golden> <alloc|sync>\n")
        return 2
    bench, scenario, golden, kind = argv[1:]
    proc = subprocess.run(
        [bench, "run", scenario, "--golden", golden],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
    )
    text = proc.stderr
    match = re.search(r"alloc_steady=(\d+) av_sync_p95_ms=(-?\d+)", text)
    if proc.returncode == 0 or match is None:
        sys.stderr.write(text)
        sys.stderr.write(proc.stdout)
        return 1
    alloc = int(match.group(1))
    sync = int(match.group(2))
    if kind == "alloc" and alloc < 1:
        sys.stderr.write(text)
        return 1
    if kind == "sync" and sync < 100:
        sys.stderr.write(text)
        return 1
    if kind not in ("alloc", "sync"):
        sys.stderr.write("unknown gate %s\n" % kind)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
