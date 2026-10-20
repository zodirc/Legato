#!/usr/bin/env bash
# Fail if liblegato exports anything outside the legato_ prefix.
set -euo pipefail

so="${1:-}"
if [[ -z "$so" || ! -f "$so" ]]; then
  echo "usage: $0 <liblegato.so>" >&2
  exit 2
fi

# Column 2 is the nm type. "A" is the version-script node (LEGATO_1.0), not a callable export.
mapfile -t symbols < <(nm -D --defined-only "$so" | awk '$2 != "A" {print $3}' | sed 's/@@.*//' | sed '/^$/d')
if [[ ${#symbols[@]} -eq 0 ]]; then
  echo "no dynamic symbols in $so" >&2
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
