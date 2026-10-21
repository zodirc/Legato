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


def corpus_command(ffmpeg, output, duration, width, height, rate, mark):
    video = "testsrc2=size=%dx%d:rate=%d:duration=%d" % (width, height, rate, duration)
    audio = "sine=frequency=1000:sample_rate=48000:duration=%d" % duration
    video_filter = "drawbox=x=%d:y=0:w=40:h=40:color=white:t=fill" % (mark * 8)
    return [
        ffmpeg,
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
        "-preset",
        "ultrafast",
        "-pix_fmt",
        "yuv420p",
        "-g",
        "30",
        "-keyint_min",
        "30",
        "-c:a",
        "aac",
        output,
    ]


def sha256_file(path):
    import hashlib

    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        while True:
            block = handle.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def generate_corpus(root, ffmpeg, jobs):
    import subprocess
    from concurrent.futures import ThreadPoolExecutor

    media = os.path.join(root, "testdata", "media")
    os.makedirs(media, exist_ok=True)
    tasks = [("local-1080p-h264.mp4", 1920, 1080, 0)]
    for index in range(30):
        tasks.append(("portrait-1080p-h264-%d.mp4" % index, 1080, 1920, index + 1))

    def one(task):
        name, width, height, mark = task
        output = os.path.join(media, name)
        command = corpus_command(ffmpeg, output, 60, width, height, 30, mark)
        print("encode", name, flush=True)
        result = subprocess.call(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if result != 0:
            raise RuntimeError("ffmpeg failed for " + name)
        return name, sha256_file(output)

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        hashed = list(pool.map(one, tasks))
    by_name = dict(hashed)
    portraits = ",".join(by_name["portrait-1080p-h264-%d.mp4" % i] for i in range(30))
    text = (
        "# Synthetic corpus. Media bytes are produced by testdata/generate_synthetic.py\n"
        "# and are not committed. Scenarios refer to these URIs.\n"
        "items:\n"
        "  - id: local-1080p-h264\n"
        "    uri: corpus://startup/local-1080p-h264\n"
        "    path: testdata/media/local-1080p-h264.mp4\n"
        "    sha256: %s\n"
        "    container: mp4\n"
        "    video_codec: h264\n"
        "    width: 1920\n"
        "    height: 1080\n"
        "    duration_s: 60\n"
        "  - id: portrait-1080p-h264\n"
        "    uri: corpus://feed/portrait-1080p-h264/*\n"
        "    path_prefix: testdata/media/portrait-1080p-h264-\n"
        "    sha256_list: %s\n"
        "    count: 30\n"
        "    container: mp4\n"
        "    video_codec: h264\n"
        "    width: 1080\n"
        "    height: 1920\n"
        "    duration_s: 60\n"
        % (by_name["local-1080p-h264.mp4"], portraits)
    )
    with open(os.path.join(root, "testdata", "corpus.yaml"), "w", encoding="utf-8") as handle:
        handle.write(text)
    print("wrote testdata/corpus.yaml", flush=True)


def main(argv):
    parser = argparse.ArgumentParser(description="generate a synthetic Legato test clip")
    parser.add_argument("--out", default="testdata/out/local-1080p-h264.mp4")
    parser.add_argument("--duration", type=int, default=5)
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--rate", type=int, default=30)
    parser.add_argument("--corpus", action="store_true")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--ffmpeg", default="")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    if args.corpus:
        root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
        ffmpeg = args.ffmpeg or os.path.join(root, "third_party", "host", "bin", "ffmpeg")
        if args.dry_run:
            sys.stdout.write(shlex.join(corpus_command(ffmpeg, "out.mp4", 60, 1920, 1080, 30, 0)) + "\n")
            return 0
        if not os.path.isfile(ffmpeg):
            sys.stderr.write("missing ffmpeg: %s\n" % ffmpeg)
            return 2
        generate_corpus(root, ffmpeg, max(1, args.jobs))
        return 0
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
