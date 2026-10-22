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


def t1_tasks():
    tasks = [("local-1080p-h264.mp4", 1920, 1080, 60, 0)]
    for index in range(30):
        tasks.append(("portrait-360p-h264-%d.mp4" % index, 360, 640, 5, index + 1))
    return tasks


def load_catalog_items(path):
    items = []
    current = None
    with open(path, "r", encoding="utf-8") as handle:
        for raw in handle:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            if line.startswith("- id:"):
                if current is not None:
                    items.append(current)
                current = {"id": line.split(":", 1)[1].strip()}
                continue
            if current is None or ":" not in line:
                continue
            key, value = line.split(":", 1)
            current[key.strip()] = value.strip()
    if current is not None:
        items.append(current)
    return items


def generate_corpus(root, ffmpeg, jobs, write_catalog):
    import subprocess
    from concurrent.futures import ThreadPoolExecutor

    media = os.path.join(root, "testdata", "media")
    os.makedirs(media, exist_ok=True)
    tasks = t1_tasks()

    def one(task):
        name, width, height, duration, mark = task
        output = os.path.join(media, name)
        if os.path.isfile(output):
            print("keep", name, flush=True)
            return name, sha256_file(output)
        command = corpus_command(ffmpeg, output, duration, width, height, 30, mark)
        print("encode", name, flush=True)
        result = subprocess.call(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if result != 0:
            raise RuntimeError("ffmpeg failed for " + name)
        return name, sha256_file(output)

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        hashed = list(pool.map(one, tasks))
    by_name = dict(hashed)
    if not write_catalog:
        print("generated T1 media; catalog not rewritten", flush=True)
        return
    feed = ",".join(by_name["portrait-360p-h264-%d.mp4" % i] for i in range(30))
    catalog = os.path.join(root, "testdata", "corpus.yaml")
    t2_hashes = ""
    if os.path.isfile(catalog):
        for item in load_catalog_items(catalog):
            if item.get("id") == "portrait-1080p-h264":
                t2_hashes = item.get("sha256_list", "")
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
        "  - id: portrait-360p-h264\n"
        "    uri: corpus://feed/portrait-360p-h264/*\n"
        "    path_prefix: testdata/media/portrait-360p-h264-\n"
        "    sha256_list: %s\n"
        "    count: 30\n"
        "    container: mp4\n"
        "    video_codec: h264\n"
        "    width: 360\n"
        "    height: 640\n"
        "    duration_s: 5\n"
        "  - id: portrait-1080p-h264\n"
        "    tier: T2\n"
        "    uri: corpus://feed/portrait-1080p-h264/*\n"
        "    path_prefix: testdata/media/portrait-1080p-h264-\n"
        "    sha256_list: %s\n"
        "    count: 30\n"
        "    container: mp4\n"
        "    video_codec: h264\n"
        "    width: 1080\n"
        "    height: 1920\n"
        "    duration_s: 60\n"
        % (by_name["local-1080p-h264.mp4"], feed, t2_hashes)
    )
    with open(catalog, "w", encoding="utf-8") as handle:
        handle.write(text)
    print("wrote testdata/corpus.yaml", flush=True)


def check_corpus(root):
    catalog = os.path.join(root, "testdata", "corpus.yaml")
    if not os.path.isfile(catalog):
        sys.stderr.write("missing %s\n" % catalog)
        return 1
    failed = 0
    for item in load_catalog_items(catalog):
        if item.get("tier") == "T2":
            continue
        if item.get("path"):
            pairs = [(item["path"], item.get("sha256", ""))]
        else:
            prefix = item.get("path_prefix", "")
            hashes = [part.strip() for part in item.get("sha256_list", "").split(",") if part.strip()]
            count = int(item.get("count", "0") or "0")
            pairs = [("%s%d.mp4" % (prefix, index), hashes[index] if index < len(hashes) else "") for index in range(count)]
        for relative, expect in pairs:
            path = os.path.join(root, relative)
            if not os.path.isfile(path):
                sys.stderr.write("missing %s\n" % relative)
                failed += 1
                continue
            actual = sha256_file(path)
            if actual != expect:
                sys.stderr.write("hash mismatch %s\n" % relative)
                failed += 1
    if failed:
        sys.stderr.write("corpus check failed: %d\n" % failed)
        return 1
    print("corpus hashes match", flush=True)
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description="generate a synthetic Legato test clip")
    parser.add_argument("--out", default="testdata/out/local-1080p-h264.mp4")
    parser.add_argument("--duration", type=int, default=5)
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--rate", type=int, default=30)
    parser.add_argument("--corpus", action="store_true")
    parser.add_argument("--write-catalog", action="store_true")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--ffmpeg", default="")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    if args.check:
        return check_corpus(root)
    if args.corpus:
        ffmpeg = args.ffmpeg or os.path.join(root, "third_party", "host", "bin", "ffmpeg")
        if args.dry_run:
            for name, width, height, duration, mark in t1_tasks():
                sys.stdout.write(shlex.join(corpus_command(ffmpeg, name, duration, width, height, 30, mark)) + "\n")
            return 0
        if not os.path.isfile(ffmpeg):
            sys.stderr.write("missing ffmpeg: %s\n" % ffmpeg)
            return 2
        generate_corpus(root, ffmpeg, max(1, args.jobs), args.write_catalog)
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
