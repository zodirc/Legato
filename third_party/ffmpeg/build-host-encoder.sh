#!/usr/bin/env bash
# 只给 generate_synthetic.py --corpus 用的编码器。不链进 liblegato。
# x264 按 third_party/x264/COMMIT 取源码，并关掉汇编。系统的 libx264 不参与，
# 码流不跟着发行版的 x264 版本和 CPU 指令集变。
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"

commit="$(tr -d '[:space:]' < third_party/x264/COMMIT)"
prefix="$(cd . && pwd)/third_party/host"
x264_src="third_party/x264/src/x264-${commit}"
tarball="third_party/x264/src/x264-${commit}.tar.bz2"
stamp="third_party/host/encoder.stamp"
config_hash="$(sha256sum third_party/ffmpeg/configs/host-encode.config third_party/x264/COMMIT | sha256sum | awk '{print $1}')"

if [[ -f "$stamp" && "$(cat "$stamp")" == "$config_hash" && -x third_party/host/bin/ffmpeg ]]; then
  if ldd third_party/host/bin/ffmpeg | grep -q 'libx264\.so'; then
    echo "host ffmpeg still links a shared libx264; rebuilding" >&2
  else
    echo "host encoder already matches ${commit}"
    exit 0
  fi
fi

mkdir -p third_party/x264/src third_party/host
if [[ ! -f "$x264_src/configure" ]]; then
  curl -L --fail -o "$tarball" \
    "https://code.videolan.org/videolan/x264/-/archive/${commit}/x264-${commit}.tar.bz2"
  tar -C third_party/x264/src -xf "$tarball"
fi
if [[ ! -f "$x264_src/configure" ]]; then
  echo "x264 source not found at $x264_src" >&2
  exit 2
fi

if [[ ! -f third_party/host/x264.commit || "$(cat third_party/host/x264.commit)" != "$commit" || ! -f third_party/host/lib/libx264.a ]]; then
  (
    cd "$x264_src"
    ./configure \
      --prefix="$prefix" \
      --enable-static \
      --enable-pic \
      --disable-cli \
      --disable-asm \
      --bit-depth=8
    make -j"$(nproc)"
    make install
  )
  printf '%s\n' "$commit" > third_party/host/x264.commit
fi

version="5.1.6"
src="third_party/ffmpeg/src/ffmpeg-${version}"
host_src="third_party/ffmpeg/src/ffmpeg-${version}-host"
ff_tar="third_party/ffmpeg/src/ffmpeg-${version}.tar.xz"
if [[ ! -x "${src}/configure" ]]; then
  mkdir -p third_party/ffmpeg/src
  curl -L --fail -o "$ff_tar" "https://ffmpeg.org/releases/ffmpeg-${version}.tar.xz"
  tar -C third_party/ffmpeg/src -xf "$ff_tar"
fi
rm -rf "$host_src"
cp -a "$src" "$host_src"

export PKG_CONFIG_PATH="${prefix}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
export LEGATO_FFMPEG_EXTRA_CFLAGS="-I${prefix}/include"
export LEGATO_FFMPEG_EXTRA_LDFLAGS="-L${prefix}/lib"
pkg-config --exists x264
./third_party/ffmpeg/build-ffmpeg.sh \
  --src "$host_src" \
  --config host-encode \
  --prefix third_party/host

if ldd third_party/host/bin/ffmpeg | grep -q 'libx264\.so'; then
  echo "host ffmpeg linked a shared libx264" >&2
  exit 1
fi
printf '%s\n' "$config_hash" > "$stamp"
echo "host encoder ready commit=${commit}"
