#!/usr/bin/env bash
# Fail if liblegato exports anything outside the legato_ prefix.
# ELF uses nm. PE uses dumpbin /exports. --from-dumpbin reads a saved listing.
set -euo pipefail

from_dumpbin=""
so=""
if [[ "${1:-}" == "--from-dumpbin" ]]; then
  from_dumpbin="${2:-}"
  if [[ -z "$from_dumpbin" || ! -f "$from_dumpbin" ]]; then
    echo "usage: $0 --from-dumpbin <dumpbin-text>" >&2
    exit 2
  fi
else
  so="${1:-}"
  if [[ -z "$so" || ! -f "$so" ]]; then
    echo "usage: $0 <liblegato.so>" >&2
    exit 2
  fi
fi

parse_dumpbin() {
  awk '
    $1 ~ /^[0-9]+$/ && $2 ~ /^[0-9A-Fa-f]+$/ && $3 ~ /^[0-9A-Fa-f]+$/ && $4 ~ /^[A-Za-z_]/ {
      name = $4
      sub(/ = .*/, "", name)
      print name
    }
  '
}

is_pe() {
  local sig
  sig="$(od -An -N2 -t x1 "$1" | tr -d ' \n')"
  [[ "$sig" == "4d5a" ]]
}

if [[ -n "$from_dumpbin" ]]; then
  mapfile -t symbols < <(parse_dumpbin < "$from_dumpbin")
elif is_pe "$so"; then
  if ! command -v dumpbin >/dev/null 2>&1; then
    echo "dumpbin is required for PE exports" >&2
    exit 1
  fi
  mapfile -t symbols < <(dumpbin /exports "$so" | parse_dumpbin)
else
  # Column 2 is the nm type. "A" is the version-script node (LEGATO_1.0), not a callable export.
  mapfile -t symbols < <(nm -D --defined-only "$so" | awk '$2 != "A" {print $3}' | sed 's/@@.*//' | sed '/^$/d')
fi

if [[ ${#symbols[@]} -eq 0 ]]; then
  echo "no dynamic symbols in ${so:-$from_dumpbin}" >&2
  exit 1
fi

found_abi=0
failed=0
for symbol in "${symbols[@]}"; do
  case "$symbol" in
    legato_*)
      if [[ "$symbol" == "legato_abi_version" ]]; then
        found_abi=1
      fi
      ;;
    *)
      echo "unexpected export: $symbol" >&2
      failed=1
      ;;
  esac
done

if [[ $found_abi -ne 1 ]]; then
  echo "legato_abi_version is not exported" >&2
  failed=1
fi

exit "$failed"
