#!/usr/bin/env bash
# 构建播放用的 FFmpeg。清单媒体按 testdata/corpus.lock 下载并核对哈希，
# 不在 CI 上编码。哈希不符或下载失败则作业失败，不回写清单。
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

version="5.1.6"
src="third_party/ffmpeg/src/ffmpeg-${version}"
demo_src="third_party/ffmpeg/src/ffmpeg-${version}-play-demo"
tarball="third_party/ffmpeg/src/ffmpeg-${version}.tar.xz"

if [[ ! -f third_party/ffmpeg/dist/lib/libavformat.a ]]; then
  mkdir -p third_party/ffmpeg/src
  if [[ ! -x "${src}/configure" ]]; then
    curl -L --fail -o "$tarball" "https://ffmpeg.org/releases/ffmpeg-${version}.tar.xz"
    tar -C third_party/ffmpeg/src -xf "$tarball"
  fi
  # 配置写在源码树里。补丁打第二次会失败，所以复制一份干净树。
  rm -rf "$demo_src"
  cp -a "$src" "$demo_src"
  ./third_party/ffmpeg/build-ffmpeg.sh \
    --src "$demo_src" \
    --config play-demo \
    --prefix third_party/ffmpeg/dist
fi

lock="testdata/corpus.lock"
if [[ ! -f "$lock" ]]; then
  echo "missing $lock" >&2
  exit 1
fi
url="$(awk -F': ' '$1=="url" {print $2}' "$lock")"
archive_sha="$(awk -F': ' '$1=="sha256" {print $2}' "$lock")"
if [[ -z "$url" || -z "$archive_sha" ]]; then
  echo "corpus.lock needs url and sha256" >&2
  exit 1
fi

archive="testdata/corpus-${archive_sha}.tar"
have_archive=0
if [[ -f "$archive" ]]; then
  actual="$(sha256sum "$archive" | awk '{print $1}')"
  if [[ "$actual" == "$archive_sha" ]]; then
    have_archive=1
  fi
fi
if [[ "$have_archive" -eq 0 ]]; then
  curl -L --fail -o "$archive.partial" "$url"
  mv "$archive.partial" "$archive"
fi
actual="$(sha256sum "$archive" | awk '{print $1}')"
if [[ "$actual" != "$archive_sha" ]]; then
  echo "corpus archive hash mismatch" >&2
  exit 1
fi
tar -C "$root" -xf "$archive"
python3 testdata/generate_synthetic.py --check
