#!/usr/bin/env python3
"""FFmpeg 的五个静态库必须是 STATIC IMPORTED，不能退回裸路径链接。"""

import pathlib
import sys

root = pathlib.Path(__file__).resolve().parents[1]
text = (root / "CMakeLists.txt").read_text(encoding="utf-8")
names = (
    "ffmpeg_avformat",
    "ffmpeg_avcodec",
    "ffmpeg_swresample",
    "ffmpeg_swscale",
    "ffmpeg_avutil",
)
failed = False
for name in names:
    if f"add_library({name} STATIC IMPORTED)" not in text:
        print(f"missing STATIC IMPORTED target {name}", file=sys.stderr)
        failed = True
    marker = f"set_target_properties({name} PROPERTIES"
    start = text.find(marker)
    if start < 0:
        print(f"missing properties for {name}", file=sys.stderr)
        failed = True
        continue
    block = text[start : start + 400]
    if "IMPORTED_LOCATION" not in block or "INTERFACE_INCLUDE_DIRECTORIES" not in block:
        print(f"{name} is not an imported library with headers", file=sys.stderr)
        failed = True
if failed:
    sys.exit(1)
