# Ae 工具链构建（Windows / PowerShell 5.1+）
#
#   .\build.ps1                      就地构建到 Apt\
#   .\build.ps1 -Clean               先删旧产物
#   .\build.ps1 -OutDir C:\ae        构建到别处
#   .\build.ps1 -Compiler clang++    指定编译器
#   .\build.ps1 -LlvmConfig <路径>   指定 llvm-config.exe（给 aec 接上进程内编译器）
#
# 找到 llvm-config 时，Clang + LLD 会被链进 aec，--aot-exe 可在进程内直接
# 编出可执行文件，不依赖任何外部编译器。
# 找不到也能构建，只是 --aot-exe 回退到外部 g++/clang++；都没有时，
# --aot 仍可生成 .cpp，自行编译即可。

param(
  [string]$OutDir      = '',
  [string]$Compiler    = '',
  [string]$LlvmConfig  = '',
  [switch]$Clean,
  [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$apt  = Join-Path $root 'Apt'
$lib  = Join-Path $apt  'lib'

function Find-Compiler([string]$want) {
    if ($want) {
        $c = Get-Command $want -ErrorAction SilentlyContinue
        if (-not $c) { throw "找不到编译器: $want" }
        return @{ exe = $c.Source; msvc = ($c.Name -match '^cl(\.exe)?$') }
    }
    foreach ($n in @('clang++', 'g++')) {
        $c = Get-Command $n -ErrorAction SilentlyContinue
        if ($c) { return @{ exe = $c.Source; msvc = $false } }
    }
    $c = Get-Command 'cl' -ErrorAction SilentlyContinue
    if ($c) { return @{ exe = $c.Source; msvc = $true } }
    throw '找不到 C++ 编译器：请安装 clang++ / g++（或 MSVC 的 cl）并加进 PATH'
}

$cc   = Find-Compiler $Compiler
$msvc = $cc.msvc
$FLAGS = if ($msvc) { @('/std:c++17', '/O2', '/EHsc', '/nologo') }
         else       { @('-std=c++17', '-O2', '-Wall') }

if ($OutDir) {
    $outTools = $OutDir
    $outLib   = Join-Path $OutDir 'lib'
} else {
    $outTools = $apt
    $outLib   = $lib
}
New-Item -ItemType Directory -Force -Path $outTools, $outLib | Out-Null
if ($Clean) {
    Get-ChildItem $outTools -Filter '*.exe' -ErrorAction SilentlyContinue | Remove-Item -Force
}

Write-Host ("编译器: {0}" -f $cc.exe)
Write-Host ("输出:   {0}" -f $outTools)

# ── 探测 LLVM ────────────────────────────────────────────────────────────────
# MSVC 链接 LLVM/Clang 静态库容易撞 ABI 与运行库冲突，只在 GNU 风格编译器下启用。
$llvmFlags = @()
$llvmLibs  = @()
$haveLlvm  = $false

if (-not $msvc) {
    $cfg = $null
    if ($LlvmConfig) {
        if (Test-Path $LlvmConfig) { $cfg = (Resolve-Path $LlvmConfig).Path }
        else { Write-Host "LLVM:   指定的 llvm-config 不存在: $LlvmConfig" }
    } else {
        $cmd = Get-Command 'llvm-config' -ErrorAction SilentlyContinue
        if ($cmd) { $cfg = $cmd.Source }
        elseif (Test-Path 'C:\Program Files\LLVM\bin\llvm-config.exe') {
            $cfg = 'C:\Program Files\LLVM\bin\llvm-config.exe'
        }
    }

    if ($cfg) {
        $haveLlvm = $true
        $ver    = (& $cfg --version).Trim()
        $libdir = (& $cfg --libdir).Trim()
        $prefix = (& $cfg --prefix).Trim()
        $bindir = (& $cfg --bindir).Trim()

        $llvmFlags += "-DAE_ENABLE_LLVM"
        $llvmFlags += "-DAE_CLANG_RESOURCE_DIR=`"$prefix/lib/clang/$($ver.Split('.')[0])`""
        $llvmFlags += "-DAE_CLANG_BINDIR=`"$bindir`""

        if (Test-Path (Join-Path $libdir 'libclangFrontend.a')) {
            $llvmLibs += '-Wl,--start-group'
            $llvmLibs += @('-lclangFrontend', '-lclangSerialization', '-lclangDriver',
                           '-lclangParse', '-lclangSema', '-lclangCodeGen',
                           '-lclangAnalysis', '-lclangEdit', '-lclangAST',
                           '-lclangLex', '-lclangBasic', '-lclangSupport',
                           '-lclangToolingCore')
            foreach ($e in @('clangAPINotes', 'clangASTMatchers', 'clangToolingSyntax',
                             'clangTransformer', 'clangRewrite', 'clangStaticAnalyzerCore',
                             'clangStaticAnalyzerCheckers', 'clangStaticAnalyzerFrontend',
                             'clangCrossTU', 'clangIndex', 'clangFormat')) {
                if (Test-Path (Join-Path $libdir "lib$e.a")) { $llvmLibs += "-l$e" }
            }
            $llvmLibs += '-Wl,--end-group'
        } else {
            $llvmLibs += '-lclang-cpp'
        }

        $lld = 0
        foreach ($n in @('ELF', 'COFF', 'MachO', 'MinGW', 'Wasm', 'Common')) {
            if (Test-Path (Join-Path $libdir "liblld$n.a")) { $llvmLibs += "-llld$n"; $lld++ }
        }
        if ($lld -eq 0) { $llvmLibs += '-lLLD' }

        $llvmLibs += (& $cfg --ldflags).Trim() -split '\s+'
        $llvmLibs += (& $cfg --libs core native).Trim() -split '\s+'
        $llvmLibs += (& $cfg --system-libs).Trim() -split '\s+'

        # LLVM 静态库不自带 zlib/zstd（llvm/llvm-project#142261）
        $llvmLibs += @('-lz', '-lzstd')

        Write-Host ("LLVM:   {0} —— Clang + LLD 链入 aec，--aot-exe 进程内编译" -f $ver)
    } else {
        Write-Host 'LLVM:   未找到 llvm-config —— --aot-exe 将回退到外部编译器'
        Write-Host '        需要进程内编译时，先装 LLVM/Clang 套件，或用 -LlvmConfig 指定'
    }
} else {
    Write-Host 'LLVM:   MSVC 下不启用 —— --aot-exe 将回退到外部编译器'
}

# ── 编译 ─────────────────────────────────────────────────────────────────────
# 编译器告警走 stderr；PS 5.1 会把原生程序的 stderr 当成 ErrorRecord，
# 配上 ErrorActionPreference=Stop，一条告警就能中断脚本。统一交给 cmd /c 代理。
$failed = @()
function Build-One([string[]]$srcs, [string]$out, [string[]]$pre, [string[]]$post) {
    $name = ($srcs | ForEach-Object { Split-Path -Leaf $_ }) -join ' + '
    Write-Host ("  [EXE ] {0,-20} → {1}" -f $name, (Split-Path -Leaf $out))
    $logFile = [System.IO.Path]::GetTempFileName()
    if ($script:msvc) {
        $args_ = @($FLAGS) + $pre + $srcs + @("/Fe:$out") + $post
    } else {
        # GNU 链接器从左到右单次扫描，-l 必须在引用它的目标文件之后
        $args_ = @($FLAGS) + $pre + $srcs + @('-o', $out) + $post
    }
    $quoted = foreach ($a in @($script:cc.exe) + $args_) {
        if ($a -match '[\s"]') { '"' + $a + '"' } else { $a }
    }
    & cmd /c (($quoted -join ' ') + ' > "' + $logFile + '" 2>&1') | Out-Null
    $rc  = $LASTEXITCODE
    $log = @()
    if (Test-Path $logFile) {
        $log = [System.IO.File]::ReadAllLines($logFile)
        Remove-Item $logFile -Force -ErrorAction SilentlyContinue
    }
    if ($rc -ne 0) {
        $script:failed += (Split-Path -Leaf $out)
        $log | Select-Object -First 12 | ForEach-Object { Write-Host "        $_" -ForegroundColor Red }
    } elseif (-not $Quiet) {
        $log | Select-Object -First 6 | ForEach-Object { Write-Host "        $_" -ForegroundColor DarkGray }
    }
}

if ($haveLlvm) {
    Build-One @((Join-Path $apt 'aec.cpp')) (Join-Path $outTools 'aec.exe') $llvmFlags $llvmLibs
} else {
    Build-One @((Join-Path $apt 'aec.cpp')) (Join-Path $outTools 'aec.exe') @() @()
}

Build-One @((Join-Path $apt 'ae.cpp'), (Join-Path $apt 'ae_main.cpp')) (Join-Path $outTools 'ae.exe') @() @()
Build-One @((Join-Path $apt 'aed.cpp')) (Join-Path $outTools 'aed.exe') @() @()

foreach ($m in @('args.m', 'json.m', 'math.m')) {
    $src = Join-Path $lib $m
    if (Test-Path $src) {
        Write-Host ("  [模块] {0,-12} → lib\" -f $m)
        $dst = Join-Path $outLib $m
        if ((Resolve-Path $src).Path -ne $dst) { Copy-Item $src $dst -Force }
    }
}

Write-Host ''
if ($failed.Count -eq 0) {
    Write-Host '构建完成。' -ForegroundColor Green
    if (-not $haveLlvm) {
        Write-Host '注意: aec 未链入编译器，--aot-exe 需要外部 g++/clang++；'
        Write-Host '      也可只用 --aot 生成 .cpp 后自行编译。'
    }
    exit 0
} else {
    Write-Host ("构建失败: {0}" -f ($failed -join ', ')) -ForegroundColor Red
    exit 1
}
