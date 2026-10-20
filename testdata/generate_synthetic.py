#!/usr/bin/env python3
"""Generate a short synthetic clip with a burned-in frame counter.

The M0 generator does not draw a QR code. testsrc2 already paints a frame
counter and a timecode; drawtext adds the same number again when a font is
available, and a white box flashes on the first frame of each second.
"""

import argparse
import os
import shlex
import shutil
import sys


def ffmpeg_command(output, duration, width, height, rate):
    video = "testsrc2=size=%dx%d:rate=%d:duration=%d" % (width, height, rate, duration)
    audio = "sine=frequency=1000:sample_rate=48000:duration=%d" % duration
    font = "/usr/share/fonts/dejavu/DejaVuSans.ttf"
    if os.path.isfile(font):
        video_filter = (
            "drawtext=fontfile=%s:text='%%{n}':x=24:y=24:fontsize=48:fontcolor=white,"
            "drawbox=x=0:y=0:w=iw:h=80:color=white:t=fill:enable='eq(mod(n,%d),0)'"
        ) % (font, rate)
    else:
        video_filter = "drawbox=x=0:y=0:w=iw:h=80:color=white:t=fill:enable='eq(mod(n,%d),0)'" % rate
    return [
        "ffmpeg",
        "-y",
        "-f",
        "lavfi",
        "-i",
        video,
        "-f",
        "lavfi",
        "-i",
        audio,
        "-vf",
        video_filter,
        "-shortest",
        "-c:v",
        "libx264",
        "-pix_fmt",
        "yuv420p",
        "-c:a",
        "aac",
        output,
    ]


def main(argv):
    parser = argparse.ArgumentParser(description="generate a synthetic Legato test clip")
    parser.add_argument("--out", default="testdata/out/local-1080p-h264.mp4")
    parser.add_argument("--duration", type=int, default=5)
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--rate", type=int, default=30)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    command = ffmpeg_command(args.out, args.duration, args.width, args.height, args.rate)
    if args.dry_run:
        sys.stdout.write(shlex.join(command) + "\n")
        return 0
    if shutil.which("ffmpeg") is None:
        sys.stderr.write("ffmpeg is not installed; pass --dry-run to print the command\n")
        return 2
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    import subprocess

    return subprocess.call(command)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
