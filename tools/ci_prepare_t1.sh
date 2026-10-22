#!/usr/bin/env bash
# Build the pinned FFmpeg slices when the CI cache missed, then generate corpus
# media and check it against testdata/corpus.yaml. A mismatch fails the job and
# does not rewrite the catalog.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

version="5.1.6"
src="third_party/ffmpeg/src/ffmpeg-${version}"
demo_src="third_party/ffmpeg/src/ffmpeg-${version}-play-demo"
host_src="third_party/ffmpeg/src/ffmpeg-${version}-host"
tarball="third_party/ffmpeg/src/ffmpeg-${version}.tar.xz"

need_demo=0
need_host=0
[[ -f third_party/ffmpeg/dist/lib/libavformat.a ]] || need_demo=1
[[ -x third_party/host/bin/ffmpeg ]] || need_host=1

if [[ "$need_demo" -eq 1 || "$need_host" -eq 1 ]]; then
  mkdir -p third_party/ffmpeg/src
  if [[ ! -x "${src}/configure" ]]; then
    curl -L -o "$tarball" "https://ffmpeg.org/releases/ffmpeg-${version}.tar.xz"
    tar -C third_party/ffmpeg/src -xf "$tarball"
  fi
fi

# 两套配置都在源码树里 configure。补丁打第二次会失败，所以各复制一份干净树。
if [[ "$need_demo" -eq 1 ]]; then
  rm -rf "$demo_src"
  cp -a "$src" "$demo_src"
  ./third_party/ffmpeg/build-ffmpeg.sh \
    --src "$demo_src" \
    --config play-demo \
    --prefix third_party/ffmpeg/dist
fi

if [[ "$need_host" -eq 1 ]]; then
  rm -rf "$host_src"
  cp -a "$src" "$host_src"
  ./third_party/ffmpeg/build-ffmpeg.sh \
    --src "$host_src" \
    --config host-encode \
    --prefix third_party/host
fi

python3 testdata/generate_synthetic.py --corpus --jobs "$(nproc)" \
  --ffmpeg third_party/host/bin/ffmpeg
python3 testdata/generate_synthetic.py --check
