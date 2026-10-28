#!/usr/bin/env bash
# 把 T1 清单媒体打成 corpus-<sha256>.tar，并写下 testdata/corpus.lock。
# 不包含 tier: T2 的媒体。上传地址写在 lock 的 url 字段，CI 按它下载。
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

python3 testdata/generate_synthetic.py --check

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/testdata/media"
cp testdata/media/local-1080p-h264.mp4 "$stage/testdata/media/"
cp testdata/media/portrait-360p-h264-*.mp4 "$stage/testdata/media/"

# 固定顺序和元数据，避免打包机器改变 tar 字节。
tar -C "$stage" --sort=name --mtime='UTC 2026-01-01' --owner=0 --group=0 --numeric-owner \
  -cf testdata/corpus-pack.tar testdata/media
sha="$(sha256sum testdata/corpus-pack.tar | awk '{print $1}')"
archive="testdata/corpus-${sha}.tar"
mv testdata/corpus-pack.tar "$archive"

url="https://github.com/zodirc/Legato/releases/download/corpus-1/corpus-${sha}.tar"
cat > testdata/corpus.lock <<EOF
url: ${url}
sha256: ${sha}
EOF
echo "wrote $archive"
echo "wrote testdata/corpus.lock"
