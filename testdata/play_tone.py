#!/usr/bin/env python3
"""Generate a short stereo tone and check both device-missing outcomes."""

import math
import os
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

env = os.environ.copy()
env["LEGATO_TEST_NO_AUDIO_DEVICE"] = "1"


def run(preset):
    started = time.monotonic()
    proc = subprocess.run([sys.argv[1], str(path), preset], env=env, capture_output=True, text=True)
    return proc, time.monotonic() - started


vod, elapsed = run("vod")
if vod.returncode != 0:
    sys.stderr.write(vod.stderr)
    raise SystemExit(vod.returncode)
if elapsed < 0.8:
    print("播放过快（%.2fs），没有按实时走完" % elapsed, file=sys.stderr)
    raise SystemExit(1)
if "结束" not in vod.stdout or "audio_fallback" not in vod.stdout:
    print(vod.stdout, file=sys.stderr)
    print("vod 没有走到流结束或没有上报 audio_fallback", file=sys.stderr)
    raise SystemExit(1)

music, _music_elapsed = run("music")
if music.returncode == 0 or "audio_device_unavailable" not in music.stderr:
    sys.stderr.write(music.stderr)
    print(music.stdout, file=sys.stderr)
    print("music 在没有 PCM 设备时应当报 audio_device_unavailable", file=sys.stderr)
    raise SystemExit(1)
