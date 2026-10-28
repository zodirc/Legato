#!/usr/bin/env bash
# Cross-build helper for the pinned FFmpeg tree.
# Legato does not fork FFmpeg. Patches live in patches/ and must stay upstreamable.
# Usage:
#   build-ffmpeg.sh --src /path/to/ffmpeg --config play-min --prefix /path/to/out
set -euo pipefail

src=""
config="play-min"
prefix=""
root="$(cd "$(dirname "$0")" && pwd)"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --src) src="$2"; shift 2 ;;
    --config) config="$2"; shift 2 ;;
    --prefix) prefix="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [[ -z "$src" || -z "$prefix" ]]; then
  echo "usage: $0 --src <ffmpeg-source> --prefix <install-prefix> [--config play-min|play-full|process]" >&2
  exit 2
fi
if [[ ! -x "$src/configure" ]]; then
  echo "ffmpeg source not found at $src (expected configure)" >&2
  exit 2
fi

cfg="$root/configs/${config}.config"
if [[ ! -f "$cfg" ]]; then
  echo "unknown config: $config" >&2
  exit 2
fi

mapfile -t flags < <(grep -v '^#' "$cfg" | grep -v '^[[:space:]]*$')
if [[ -n "${LEGATO_FFMPEG_EXTRA_CFLAGS:-}" ]]; then
  flags+=("--extra-cflags=${LEGATO_FFMPEG_EXTRA_CFLAGS}")
fi
if [[ -n "${LEGATO_FFMPEG_EXTRA_LDFLAGS:-}" ]]; then
  flags+=("--extra-ldflags=${LEGATO_FFMPEG_EXTRA_LDFLAGS}")
fi
shopt -s nullglob
patches=("$root"/patches/*.patch)
if [[ ${#patches[@]} -gt 0 ]]; then
  for patch_file in "${patches[@]}"; do
    echo "apply ${patch_file##*/}"
    patch -d "$src" -p1 --forward --batch < "$patch_file"
  done
fi
mkdir -p "$prefix"
prefix="$(cd "$prefix" && pwd)"
(
  cd "$src"
  ./configure --prefix="$prefix" "${flags[@]}"
  make -j"$(nproc)"
  make install
)

echo "built ffmpeg config=$config prefix=$prefix"
echo "link the static libs into liblegato and keep the version script third_party/ffmpeg/liblegato.version"
