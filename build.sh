#!/usr/bin/env bash
# Ae 工具链构建（Linux / macOS / Termux）
#
#   ./build.sh                 就地构建到 Apt/
#   ./build.sh --clean         先删旧产物
#   ./build.sh --no-aed        只编 aec 与 ae
#   OUT=/tmp/ae ./build.sh     构建到别处
#   CXX=clang++ ./build.sh     换编译器
#   LLVM_CONFIG=<路径> ...     指定 llvm-config（给 aec 接上进程内编译器）
#
# 有 llvm-config 时，Clang + LLD 会被链进 aec，于是 --aot-exe 能在进程内
# 直接编出可执行文件，不依赖任何外部编译器。
# 没有也能正常构建，只是 --aot-exe 会回退到外部 g++/clang++；
# 连外部编译器都没有时，--aot 仍可生成 .cpp，自己拿去编。
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
done

mkdir -p "$OUT_TOOLS" "$OUT_LIB"

echo "编译器: $CXX"
echo "输出:   $OUT_TOOLS"

# ── 探测 LLVM ────────────────────────────────────────────────────────────────
LLVM_CONFIG="${LLVM_CONFIG:-llvm-config}"
AE_LLVM_FLAGS=()
AE_LLVM_LIBS=()
HAVE_LLVM=0

if command -v "$LLVM_CONFIG" >/dev/null 2>&1; then
  HAVE_LLVM=1
  LLVM_VER="$("$LLVM_CONFIG" --version)"
  LLVM_LIBDIR="$("$LLVM_CONFIG" --libdir)"
  LLVM_PREFIX="$("$LLVM_CONFIG" --prefix)"

  AE_LLVM_FLAGS+=(
    -DAE_ENABLE_LLVM
    -DAE_CLANG_RESOURCE_DIR="\"$LLVM_PREFIX/lib/clang/${LLVM_VER%%.*}\""
    -DAE_CLANG_BINDIR="\"$("$LLVM_CONFIG" --bindir)\""
  )

  # Clang 各组件互相依赖（Sema→APINotes、Analysis→ASTMatchers），
  # 顺序排不完，必须让链接器反复扫描。
  if ls "$LLVM_LIBDIR"/libclangFrontend.* >/dev/null 2>&1; then
    AE_LLVM_LIBS+=(-Wl,--start-group)
    AE_LLVM_LIBS+=(
      -lclangFrontend -lclangSerialization -lclangDriver -lclangParse
      -lclangSema -lclangCodeGen -lclangAnalysis -lclangEdit
      -lclangAST -lclangLex -lclangBasic -lclangSupport -lclangToolingCore
    )
    for extra in clangAPINotes clangASTMatchers clangToolingSyntax \
                 clangTransformer clangRewrite clangStaticAnalyzerCore \
                 clangStaticAnalyzerCheckers clangStaticAnalyzerFrontend \
                 clangCrossTU clangIndex clangFormat; do
      # 单个 ls：写成 `ls a b c` 时任一缺失就会整体判假
      ls "$LLVM_LIBDIR"/lib${extra}.a >/dev/null 2>&1 && AE_LLVM_LIBS+=(-l${extra})
    done
    AE_LLVM_LIBS+=(-Wl,--end-group)
  else
    AE_LLVM_LIBS+=(-lclang-cpp)
  fi

  LLD_COUNT=0
  for n in ELF COFF MachO MinGW Wasm Common; do
    if ls "$LLVM_LIBDIR"/liblld${n}.* >/dev/null 2>&1; then
      AE_LLVM_LIBS+=(-llld${n}); ((++LLD_COUNT))
    fi
  done
  [[ $LLD_COUNT -eq 0 ]] && AE_LLVM_LIBS+=(-lLLD)

  if "$LLVM_CONFIG" --link-shared --libs core native >/dev/null 2>&1; then
    AE_LLVM_LIBS+=("$("$LLVM_CONFIG" --ldflags)" \
                   "$("$LLVM_CONFIG" --link-shared --libs core native)")
  else
    AE_LLVM_LIBS+=("$("$LLVM_CONFIG" --ldflags)" \
                   "$("$LLVM_CONFIG" --libs core native)")
  fi
  AE_LLVM_LIBS+=("$("$LLVM_CONFIG" --system-libs)")

  # LLVM 静态库不自带 zlib/zstd（llvm/llvm-project#142261），必须显式给
  AE_LLVM_LIBS+=(-lz -lzstd)

  echo "LLVM:   $LLVM_VER —— Clang + LLD 链入 aec，--aot-exe 进程内编译"
else
  echo "LLVM:   未找到 llvm-config —— --aot-exe 将回退到外部编译器"
  echo "        需要进程内编译时，先装 LLVM/Clang 套件，或用 LLVM_CONFIG=<路径> 指定"
fi

build_exe() {
  echo "  [EXE ] $(basename "$1") → $(basename "$2")"
  "$CXX" "${FLAGS[@]}" "$1" -o "$2"
}

# ── 工具链 ───────────────────────────────────────────────────────────────────
if [[ $HAVE_LLVM -eq 1 ]]; then
  echo "  [EXE ] aec.cpp + Clang/LLD → aec"
  "$CXX" "${FLAGS[@]}" "${AE_LLVM_FLAGS[@]}" "$APT/aec.cpp" -o "$OUT_TOOLS/aec" \
      "${AE_LLVM_LIBS[@]}"
else
  build_exe "$APT/aec.cpp" "$OUT_TOOLS/aec"
fi

echo "  [EXE ] ae.cpp + ae_main.cpp → ae"
"$CXX" "${FLAGS[@]}" "$APT/ae.cpp" "$APT/ae_main.cpp" -o "$OUT_TOOLS/ae"

if [[ $WANT_AED -eq 1 ]]; then
  build_exe "$APT/aed.cpp" "$OUT_TOOLS/aed"
fi

if [[ "$OUT_LIB" != "$LIB" ]]; then
  for m in args.m json.m math.m; do
    [[ -f "$LIB/$m" ]] || continue
    echo "  [模块] $m → lib/"
    cp -f "$LIB/$m" "$OUT_LIB/$m"
  done
fi

chmod +x "$OUT_TOOLS"/aec "$OUT_TOOLS"/ae "$OUT_TOOLS"/aed 2>/dev/null

echo
echo "构建完成。"
if [[ $HAVE_LLVM -eq 0 ]]; then
  echo "注意: aec 未链入编译器，--aot-exe 需要外部 g++/clang++；"
  echo "      也可只用 --aot 生成 .cpp 后自行编译。"
fi
