#!/usr/bin/env python3
"""引擎和基准写出的 JSON 不用 printf 的浮点转换。区域设置会把小数点换成逗号。"""

import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parents[1]
pattern = re.compile(r"%[-+0-9.#]*[fFeEgG]")
dirs = ("api", "bench", "codec", "core", "demux", "engine", "io", "pal", "render")
failed = False
for name in dirs:
    base = root / name
    if not base.is_dir():
        continue
    for path in base.rglob("*"):
        if path.suffix not in {".c", ".cpp", ".h", ".hpp"}:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if pattern.search(text):
            print(f"printf float conversion in {path.relative_to(root)}", file=sys.stderr)
            failed = True
bench = (root / "bench/src/bench_main.cpp").read_text(encoding="utf-8")
if "json_fixed6" not in bench:
    print("bench_main.cpp does not call json_fixed6", file=sys.stderr)
    failed = True
if failed:
    sys.exit(1)
