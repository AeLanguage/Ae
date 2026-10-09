#!/usr/bin/env bash
# =============================================================================
#  Ae 工具链构建（Linux / macOS）—— 与 build.ps1 对应
# -----------------------------------------------------------------------------
#  【头文件版标准库】io / os / path 三个库已经改成头文件（Apt/lib/ae_lib_*.h），
#  由 Apt/ae_builtin.h 直接 #include，在编译工具链时就链进可执行文件。
#  因此构建产物只有：
#      Apt/aec.cpp  → aec   编译器
#      Apt/ae.cpp   → ae    虚拟机
#      Apt/aed.cpp  → aed   反汇编器（可选，加 --with-aed，默认也编）
#
#  不再生成任何 .so / .dylib / .dll，也不需要把它们放到某个目录里。
#  Apt/lib/args.m 与 json.m 是用 Ae 自己写的模块（不是动态库），
#  不需要编译，只要放在 <exe>/lib/ 或用 AE_LIB_PATH 指到它所在目录即可。
#
#  用法：
#      ./build.sh                 # 就地构建到 Apt/
#      ./build.sh --clean         # 先删旧产物
#      ./build.sh --no-aed        # 只编 aec 与 ae
#      OUT=/tmp/ae ./build.sh     # 构建到别处
#      CXX=clang++ ./build.sh     # 换编译器
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APT="$ROOT/Apt"
LIB="$APT/lib"
CXX="${CXX:-g++}"
FLAGS=(-std=c++17 -O2 -Wall)

OUT_TOOLS="${OUT:-$APT}"
OUT_LIB="$OUT_TOOLS/lib"
WANT_AED=1

for a in "$@"; do
  case "$a" in
    --clean)   ;;
    --no-aed)  WANT_AED=0 ;;
    *)         echo "未知参数: $a"; exit 2 ;;
  esac
done
for a in "$@"; do
  [[ "$a" == "--clean" ]] || continue
  rm -f "$OUT_TOOLS"/aec "$OUT_TOOLS"/ae "$OUT_TOOLS"/aed
  rm -f "$OUT_LIB"/*.so "$OUT_LIB"/*.dylib "$OUT_LIB"/*.dll 2>/dev/null || true
done

mkdir -p "$OUT_TOOLS" "$OUT_LIB"

echo "编译器: $CXX"
echo "输出:   $OUT_TOOLS   （内置库 io/os/path 已静态链入，无需库文件）"

build_exe() {
  echo "  [EXE ] $(basename "$1") → $(basename "$2")"
  "$CXX" "${FLAGS[@]}" "$1" -o "$2"
}

# ── 工具链 ──────────────────────────────────────────────────────────────────
build_exe "$APT/aec.cpp" "$OUT_TOOLS/aec"
build_exe "$APT/ae.cpp"  "$OUT_TOOLS/ae"
if [[ $WANT_AED -eq 1 ]]; then
  build_exe "$APT/aed.cpp" "$OUT_TOOLS/aed"
fi

# ── Ae 模块：不用编译，拷过去就能 imp ───────────────────────────────────────
if [[ "$OUT_LIB" != "$LIB" ]]; then
  for m in args.m json.m; do
    [[ -f "$LIB/$m" ]] || continue
    echo "  [模块] $m → lib/"
    cp -f "$LIB/$m" "$OUT_LIB/$m"
  done
fi

echo
echo "构建完成（只有可执行文件，没有 .so / .dll）。试一下："
echo "  $OUT_TOOLS/aec -h"
echo "  echo 'func main() { prln(\"你好, 世界\") }' > /tmp/hello.ae && $OUT_TOOLS/aec /tmp/hello.ae && $OUT_TOOLS/ae /tmp/hello.aeo"
