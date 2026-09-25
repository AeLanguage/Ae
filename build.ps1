# =============================================================================
#  Ae 工具链一键构建（Windows / PowerShell 5.1+）
# -----------------------------------------------------------------------------
#  会编译 7 个东西：
#     工具（可执行）  Apt\aec.cpp       → aec.exe   编译器
#                     Apt\ae.cpp        → ae.exe    虚拟机
#                     Apt\aed.cpp       → aed.exe   反汇编器
#     原生库（DLL）   Apt\lib\io.cpp    → lib\io.dll
#                     Apt\lib\os.cpp    → lib\os.dll
#                     Apt\lib\path.cpp  → lib\path.dll
#                     Apt\lib\ui.cpp    → lib\ui.dll    （用 Windows 的 GDI+，多链 4 个系统库）
#     Apt\lib\args.m 与 json.m 是 Ae 写的模块，不需要编译，拷到 lib\ 就能 imp。
#
#  用法：
#      powershell -ExecutionPolicy Bypass -File build.ps1
#      powershell -ExecutionPolicy Bypass -File build.ps1 -Clean
#      powershell -ExecutionPolicy Bypass -File build.ps1 -OutDir D:\tmp\ae
#      powershell -ExecutionPolicy Bypass -File build.ps1 -Compiler g++
#
#  默认就地构建到 Apt\ 与 Apt\lib\ —— Test\run.ps1 就是从那里找工具的。
# =============================================================================
param(
  [string]$OutDir   = '',     # 空 = 就地（Apt\ + Apt\lib\）
  [string]$Compiler = '',     # 空 = 自动挑 clang++ / g++ / cl
  [switch]$Clean,
  [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$apt  = Join-Path $root 'Apt'
$lib  = Join-Path $apt  'lib'

# ── 挑编译器：clang++ → g++ → cl ─────────────────────────────────────────────
function Find-Compiler([string]$want) {
    if ($want) {
        $c = Get-Command $want -ErrorAction SilentlyContinue
        if (-not $c) { throw "找不到编译器: $want （请先装好并加进 PATH）" }
        $leaf = Split-Path -Leaf $c.Source
        $kind = 'gnu'
        if ($leaf -eq 'cl' -or $leaf -eq 'cl.exe') { $kind = 'msvc' }
        return @{ exe = $c.Source; kind = $kind }
    }
    foreach ($n in @('clang++', 'g++')) {
        $c = Get-Command $n -ErrorAction SilentlyContinue
        if ($c) { return @{ exe = $c.Source; kind = 'gnu' } }
    }
    $c = Get-Command 'cl' -ErrorAction SilentlyContinue
    if ($c) { return @{ exe = $c.Source; kind = 'msvc' } }
    throw '找不到 C++ 编译器：请安装 clang++ / g++（或 MSVC 的 cl），并把它的目录加进 PATH'
}

$cc = Find-Compiler $Compiler
$msvc = ($cc.kind -eq 'msvc')
$FLAGS = if ($msvc) { @('/std:c++17', '/O2', '/EHsc', '/nologo') } else { @('-std=c++17', '-O2', '-Wall') }

# ── 输出目录 ────────────────────────────────────────────────────────────────
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
    Get-ChildItem $outLib   -Filter '*.dll' -ErrorAction SilentlyContinue | Remove-Item -Force
}

Write-Host ("编译器: {0}" -f $cc.exe) -ForegroundColor Cyan
Write-Host ("输出:   {0}   +   {1}" -f $outTools, $outLib) -ForegroundColor Cyan

$failed = @()
function Build-One([string]$kindName, [string]$src, [string]$out, [string[]]$extra) {
    $cargs = @()
    if ($msvc) {
        if ($kindName -eq 'DLL') { $cargs += $FLAGS + @('/LD') }
        else                     { $cargs += $FLAGS }
        $cargs += @($src, "/Fe:$out") + $extra
    } else {
        if ($kindName -eq 'DLL') { $cargs += $FLAGS + @('-shared') }
        else                     { $cargs += $FLAGS }
        $cargs += @($src, '-o', $out) + $extra
    }
    Write-Host ("  [{0,-4}] {1,-10} → {2}" -f $kindName, (Split-Path -Leaf $src), (Split-Path -Leaf $out))
    # ★ 编译器告警走 stderr；PS 5.1 里原生程序的 stderr 会被当成 ErrorRecord，配上
    #   $ErrorActionPreference='Stop'，一条普通告警就能让整个脚本当场停住。所以统一
    #   交给 cmd /c 执行并重定向到临时文件，PowerShell 完全不碰它的输出流。
    $logFile = [System.IO.Path]::GetTempFileName()
    $quoted = foreach ($a in @($cc.exe) + $cargs) { if ($a -match '[\s"]') { '"' + $a + '"' } else { $a } }
    $cmdline = ($quoted -join ' ') + ' > "' + $logFile + '" 2>&1'
    & cmd /c $cmdline | Out-Null
    $rc = $LASTEXITCODE
    $log = @()
    if (Test-Path $logFile) { $log = [System.IO.File]::ReadAllLines($logFile); Remove-Item $logFile -Force -ErrorAction SilentlyContinue }
    if ($rc -ne 0) {
        $script:failed += (Split-Path -Leaf $src)
        $log | Select-Object -First 12 | ForEach-Object { Write-Host "        $_" -ForegroundColor Red }
    } elseif (-not $Quiet) {
        $log | Select-Object -First 6 | ForEach-Object { Write-Host "        $_" -ForegroundColor DarkGray }
    }
}

# ── 工具链 ──────────────────────────────────────────────────────────────────
Build-One 'EXE' (Join-Path $apt 'aec.cpp') (Join-Path $outTools 'aec.exe') @()
Build-One 'EXE' (Join-Path $apt 'ae.cpp')  (Join-Path $outTools 'ae.exe')  @()
Build-One 'EXE' (Join-Path $apt 'aed.cpp') (Join-Path $outTools 'aed.exe') @()

# ── 原生库（ui 需要 GDI+，多链 user32/gdi32/gdiplus/winmm）──────────────────
Build-One 'DLL' (Join-Path $lib 'io.cpp')   (Join-Path $outLib 'io.dll')   @()
Build-One 'DLL' (Join-Path $lib 'os.cpp')   (Join-Path $outLib 'os.dll')   @()
Build-One 'DLL' (Join-Path $lib 'path.cpp') (Join-Path $outLib 'path.dll') @()
if ($msvc) {
    Build-One 'DLL' (Join-Path $lib 'ui.cpp') (Join-Path $outLib 'ui.dll') @('user32.lib','gdi32.lib','gdiplus.lib','winmm.lib')
} else {
    Build-One 'DLL' (Join-Path $lib 'ui.cpp') (Join-Path $outLib 'ui.dll') @('-luser32','-lgdi32','-lgdiplus','-lwinmm')
}

# ── Ae 模块：不用编译，拷到 lib\ ────────────────────────────────────────────
foreach ($m in @('args.m', 'json.m')) {
    $src = Join-Path $lib $m
    if (Test-Path $src) {
        Write-Host ("  [模块] {0,-10} → lib\" -f $m)
        $dst = Join-Path $outLib $m
        if ((Resolve-Path $src).Path -ne $dst) { Copy-Item $src $dst -Force }
    }
}

# ── 清掉 clang 生成的导入库（.lib）：Ae 是运行时 LoadLibrary 找 DLL，用不到 ──
Get-ChildItem $outLib -Filter '*.lib' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

# ── 结果 ────────────────────────────────────────────────────────────────────
Write-Host ''
if ($failed.Count -eq 0) {
    Write-Host '构建完成。' -ForegroundColor Green
    Write-Host ("  用法: {0} -h" -f (Join-Path $outTools 'aec.exe'))
    Write-Host ("  自测: powershell -ExecutionPolicy Bypass -File `"{0}`"" -f (Join-Path $root 'Test\run.ps1'))
    exit 0
} else {
    Write-Host ("构建失败: {0}" -f ($failed -join ', ')) -ForegroundColor Red
    exit 1
}
