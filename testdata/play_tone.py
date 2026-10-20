#!/usr/bin/env python3
"""Generate a short stereo tone and play it through legato_play."""

import math
import struct
import subprocess
import sys
import time
import wave
from pathlib import Path

root = Path(__file__).resolve().parents[1]
out_dir = root / "testdata" / "out"
out_dir.mkdir(parents=True, exist_ok=True)
path = out_dir / "tone.wav"
rate = 48000
seconds = 1.2
count = int(rate * seconds)

with wave.open(str(path), "w") as handle:
    handle.setnchannels(2)
    handle.setsampwidth(2)
    handle.setframerate(rate)
    frames = bytearray()
    for index in range(count):
        sample = int(8000 * math.sin(2 * math.pi * 440 * index / rate))
        frames += struct.pack("<hh", sample, sample)
    handle.writeframes(frames)

if len(sys.argv) != 2:
    print("usage: play_tone.py <legato_play>", file=sys.stderr)
    sys.exit(2)

started = time.monotonic()
rc = subprocess.call([sys.argv[1], str(path)])
elapsed = time.monotonic() - started
if rc != 0:
    raise SystemExit(rc)
if elapsed < 0.8:
    print("播放过快（%.2fs），没有按实时走完" % elapsed, file=sys.stderr)
    raise SystemExit(1)
