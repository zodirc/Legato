#!/usr/bin/env bash
# One entry for configure, build, and test. Picks a C++20 compiler and prints each stage.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

if [[ -t 1 ]]; then
  c_title=$'\033[1;36m'
  c_ok=$'\033[1;32m'
  c_off=$'\033[0m'
else
  c_title=""
  c_ok=""
  c_off=""
fi

say() { printf '%s==> %s%s\n' "$c_title" "$*" "$c_off"; }
good() { printf '%s%s%s\n' "$c_ok" "$*" "$c_off"; }

usage() {
  cat <<'EOF'
用法: ./tools/build.sh <命令>

  build      配置并编译（默认）
  test       编译并运行全部测试
  bench      跑两条 T1 黄金场景
  configure  只生成构建目录
  clean      删除 build/

仓库根目录也可以执行 make、make test、make bench。
EOF
}

pick_compiler() {
  local major=""
  if command -v g++ >/dev/null 2>&1; then
    major="$(g++ -dumpversion | cut -d. -f1)"
    if [[ "$major" -ge 10 ]]; then
      cxx="g++"
      cc="gcc"
      return
    fi
  fi
  if command -v clang++ >/dev/null 2>&1; then
    cxx="clang++"
    cc="clang"
    return
  fi
  echo "需要 GCC 10+ 或 Clang 才能编 C++20" >&2
  exit 1
}

configure() {
  local cxx="" cc="" generator="Unix Makefiles" jobs
  pick_compiler
  if command -v ninja >/dev/null 2>&1; then
    generator="Ninja"
  fi
  jobs="$(nproc 2>/dev/null || echo 4)"
  say "配置  编译器 ${cxx}  生成器 ${generator}  并行 ${jobs}"
  cmake -S "$root" -B "$root/build" -G "$generator" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER="$cc" \
    -DCMAKE_CXX_COMPILER="$cxx" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  if [[ -f "$root/build/compile_commands.json" ]]; then
    ln -sfn build/compile_commands.json "$root/compile_commands.json"
  fi
}

ensure() {
  if [[ ! -f "$root/build/CMakeCache.txt" ]]; then
    configure
  fi
}

do_build() {
  local jobs
  jobs="$(nproc 2>/dev/null || echo 4)"
  ensure
  say "编译"
  cmake --build "$root/build" -j "$jobs"
  good "产物"
  printf '  %s\n' \
    "$root/build/liblegato.so" \
    "$root/build/legato-bench" \
    "$root/build/legato_hello"
}

do_test() {
  do_build
  say "测试"
  ctest --test-dir "$root/build" --output-on-failure
  good "测试通过"
}

do_bench() {
  do_build
  say "T1 基准"
  "$root/build/legato-bench" run "$root/bench/scenarios/startup-local.yaml" \
    --golden "$root/bench/golden/startup-local.json"
  "$root/build/legato-bench" run "$root/bench/scenarios/feed-swipe-4g.yaml" \
    --golden "$root/bench/golden/feed-swipe-4g.json"
  good "黄金结果一致"
}

cmd="${1:-build}"
case "$cmd" in
  help|-h|--help) usage ;;
  configure) configure ;;
  build) do_build ;;
  test) do_test ;;
  bench) do_bench ;;
  clean)
    rm -rf "$root/build" "$root/compile_commands.json"
    good "已删除 build/"
    ;;
  *)
    usage
    exit 2
    ;;
esac
