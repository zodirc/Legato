#!/usr/bin/env python3
"""每秒第一帧画一条白带，同一秒开头 50 毫秒为 1 kHz 蜂鸣，其余为静音。

白带和蜂鸣在同一时刻写进文件，用来对照。内部音画偏差仍由 legato_av_sync_file 测量。
清单媒体不走这条命令，corpus 哈希不变。
"""

import os
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
ffmpeg = root / "third_party" / "host" / "bin" / "ffmpeg"
clip = root / "testdata" / "out" / "flash-beep.mp4"


def fail(text):
    print("FAIL flash_beep: %s" % text, file=sys.stderr)
    return 1


def run_ffmpeg(args):
    result = subprocess.run([str(ffmpeg)] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        return None
    return result.stdout


def metadata(stdout, key):
    prefix = key + "="
    for line in stdout.splitlines():
        text = line.strip()
        if text.startswith(prefix):
            return text[len(prefix):]
    return None


def encode():
    clip.parent.mkdir(parents=True, exist_ok=True)
    video = "testsrc2=size=360x640:rate=30:duration=3"
    audio = "sine=frequency=1000:sample_rate=48000:duration=3,volume=volume='if(lt(mod(t,1),0.05),1,0)':eval=frame"
    drawn = run_ffmpeg([
        "-hide_banner", "-y",
        "-f", "lavfi", "-i", video,
        "-f", "lavfi", "-i", audio,
        "-vf", "drawbox=x=0:y=0:w=iw:h=80:color=white:t=fill:enable='eq(mod(n,30),0)'",
        "-shortest",
        "-c:v", "libx264", "-pix_fmt", "yuv420p",
        "-c:a", "aac",
        "-t", "3",
        str(clip),
    ])
    return drawn is not None


def yavg(frame_index):
    stdout = run_ffmpeg([
        "-hide_banner", "-i", str(clip),
        "-vf", "select=eq(n\\,%d),crop=iw:80:0:0,signalstats,metadata=print:file=-" % frame_index,
        "-an", "-f", "null", "-",
    ])
    if stdout is None:
        return None
    value = metadata(stdout, "lavfi.signalstats.YAVG")
    if value is None:
        return None
    return float(value)


def rms_level(start, end):
    stdout = run_ffmpeg([
        "-hide_banner", "-i", str(clip),
        "-af", "atrim=%s:%s,astats=metadata=1,ametadata=print:file=-" % (start, end),
        "-vn", "-f", "null", "-",
    ])
    if stdout is None:
        return None
    return metadata(stdout, "lavfi.astats.1.RMS_level")


def media_ok():
    flash = yavg(0)
    plain = yavg(1)
    if flash is None or plain is None or flash < 200 or plain > 180:
        return fail("white bar is not only on the first frame of each second")
    beep = rms_level("0", "0.05")
    quiet = rms_level("0.2", "0.4")
    if beep is None or beep == "-inf":
        return fail("missing beep")
    if float(beep) < -40:
        return fail("beep is too quiet")
    if quiet is None or quiet != "-inf":
        return fail("audio between beeps is not silent")
    return 0


def main(argv):
    if len(argv) != 2 or argv[1] not in ("ok", "offset"):
        print("usage: flash_beep.py <legato_av_sync_file> ok|offset", file=sys.stderr)
        return 2
    if not ffmpeg.is_file():
        return fail("missing %s" % ffmpeg)
    if not encode():
        return fail("encode")
    if media_ok() != 0:
        return 1
    return subprocess.call([argv[0], str(clip), argv[1]], env=os.environ.copy())


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
