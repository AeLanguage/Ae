#!/usr/bin/env bash
# =============================================================================
#  Ae 工具链构建（Linux / macOS）—— 与 build.ps1 对应
# -----------------------------------------------------------------------------
#  编译三件套（可执行）：
#      Apt/aec.cpp  → aec   编译器
#      Apt/ae.cpp   → ae    虚拟机
#      Apt/aed.cpp  → aed   反汇编器
#  以及跨平台的原生库（.so / .dylib）：
#      Apt/lib/io.cpp    → lib/libio.so
#      Apt/lib/os.cpp    → lib/libos.so
#      Apt/lib/path.cpp  → lib/libpath.so
#
#  注意：ui 库用的是 Windows 的 GDI+，**只能在 Windows 上编译**，本脚本会跳过它。
#        Apt/lib/args.m 与 json.m 是 Ae 写的模块，不需要编译，拷到 lib/ 即可。
#
#  用法：
#      ./build.sh                 # 就地构建到 Apt/ 与 Apt/lib/
#      ./build.sh --clean         # 先删旧产物
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
mkdir -p "$OUT_TOOLS" "$OUT_LIB"

if [[ "${1:-}" == "--clean" ]]; then
  rm -f "$OUT_TOOLS"/aec "$OUT_TOOLS"/ae "$OUT_TOOLS"/aed
  rm -f "$OUT_LIB"/*.so "$OUT_LIB"/*.dylib 2>/dev/null || true
fi

echo "编译器: $CXX"
echo "输出:   $OUT_TOOLS   +   $OUT_LIB"

build_exe() {
  echo "  [EXE ] $(basename "$1") → $(basename "$2")"
  "$CXX" "${FLAGS[@]}" "$1" -o "$2"
}
build_shlib() {
  local src="$1" out="$2"; shift 2
  echo "  [SHARED] $(basename "$src") → $(basename "$out")"
  "$CXX" "${FLAGS[@]}" -shared -fPIC "$src" -o "$out" "$@"
}

# ── 工具链 ──────────────────────────────────────────────────────────────────
build_exe "$APT/aec.cpp" "$OUT_TOOLS/aec"
build_exe "$APT/ae.cpp"  "$OUT_TOOLS/ae"
build_exe "$APT/aed.cpp" "$OUT_TOOLS/aed"

# ── 跨平台原生库（ui 是 Windows 专用，这里跳过）────────────────────────────
case "$(uname -s)" in
  Darwin) SO=dylib ;;
  *)      SO=so ;;
esac
build_shlib "$LIB/io.cpp"   "$OUT_LIB/libio.$SO"
build_shlib "$LIB/os.cpp"   "$OUT_LIB/libos.$SO"
build_shlib "$LIB/path.cpp" "$OUT_LIB/libpath.$SO"

if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || "$(uname -s)" == CYGWIN* ]]; then
  echo "  [SHARED] ui.cpp → ui.dll"
  "$CXX" "${FLAGS[@]}" -shared "$LIB/ui.cpp" -o "$OUT_LIB/ui.dll" \
      -luser32 -lgdi32 -lgdiplus -lwinmm
fi

# ── Ae 模块：不用编译 ───────────────────────────────────────────────────────
for m in args.m json.m; do
  [[ -f "$LIB/$m" ]] || continue
  echo "  [模块] $m → lib/"
  if [[ "$OUT_LIB" != "$LIB" ]]; then cp -f "$LIB/$m" "$OUT_LIB/$m"; fi
done

echo
echo "构建完成。试一下："
echo "  $OUT_TOOLS/aec -h"
echo "  echo 'func main() { prln(\"你好, 世界\") }' > /tmp/hello.ae && $OUT_TOOLS/aec /tmp/hello.ae && $OUT_TOOLS/ae /tmp/hello.aeo"
