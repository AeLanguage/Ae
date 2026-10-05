# =============================================================================
#  Ae 回归测试运行器
#  用法（在 Test/ 目录下）：.\run.ps1   或   powershell -ExecutionPolicy Bypass -File run.ps1
#  可选过滤：.\run.ps1 -Filter p13
#
#  约定：
#    cases/<name>.ae    必须编译并成功运行，stdout 与 cases/<name>.out 完全一致
#    errors/<name>.ae   必须失败（编译期或运行期），stderr 含 errors/<name>.err 的文本
#
#  注意：测试跑的是 Apt/ 里的工具链。改了 Apt/*.cpp 之后请先重新编译并覆盖
#        Apt/aec.exe、Apt/ae.exe，否则测的还是旧版本。
# =============================================================================
param([string]$Filter = '')

$ErrorActionPreference = 'Stop'
$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$apt    = Join-Path (Split-Path -Parent $here) 'Apt'
$aec    = Join-Path $apt 'aec.exe'
$ae     = Join-Path $apt 'ae.exe'
$utf8   = New-Object System.Text.UTF8Encoding($false)

foreach ($exe in @($aec, $ae)) {
    if (-not (Test-Path $exe)) { Write-Host "缺少工具链: $exe" -ForegroundColor Red; exit 2 }
}

function Norm([string]$s) { if ($null -eq $s) { '' } else { ($s -replace "`r`n", "`n").TrimEnd("`n") } }

$pass = 0; $fail = 0; $failed = @()

# ── 1) 正向用例 ──
$caseDir = Join-Path $here 'cases'
foreach ($aeFile in Get-ChildItem $caseDir -Filter '*.ae' | Sort-Object Name) {
    $name = $aeFile.BaseName
    if ($Filter -and $name -notlike "*$Filter*") { continue }
    $expPath = Join-Path $caseDir "$name.out"
    Push-Location $caseDir
    cmd /c "`"$aec`" `"$name.ae`" >nul 2> `"$name.compile.tmp`""
    $compiled = ($LASTEXITCODE -eq 0)
    $actual = ''
    if ($compiled) {
        cmd /c "`"$ae`" `"$name.aeo`" > `"$name.run.tmp`" 2>nul"
        if (Test-Path "$name.run.tmp") { $actual = [System.IO.File]::ReadAllText((Join-Path $caseDir "$name.run.tmp"), $utf8) }
    } else {
        if (Test-Path "$name.compile.tmp") { $actual = [System.IO.File]::ReadAllText((Join-Path $caseDir "$name.compile.tmp"), $utf8) }
    }
    Remove-Item "$name.aeo","$name.run.tmp","$name.compile.tmp" -ErrorAction SilentlyContinue
    Pop-Location

    $expected = if (Test-Path $expPath) { [System.IO.File]::ReadAllText($expPath, $utf8) } else { $null }
    if (-not $compiled) {
        $fail++; $failed += "$name (编译失败: $((Norm $actual) -replace "`n", ' '))"
    } elseif ($null -eq $expected) {
        $fail++; $failed += "$name (缺少 $name.out)"
    } elseif ((Norm $actual) -eq (Norm $expected)) {
        $pass++
    } else {
        $fail++; $failed += "$name (输出不一致)"
    }
}

# ── 2) 失败用例 ──
$errDir = Join-Path $here 'errors'
foreach ($aeFile in Get-ChildItem $errDir -Filter '*.ae' | Sort-Object Name) {
    $name = $aeFile.BaseName
    if ($Filter -and $name -notlike "*$Filter*") { continue }
    $expPath = Join-Path $errDir "$name.err"
    Push-Location $errDir
    cmd /c "`"$aec`" `"$name.ae`" >nul 2> `"$name.tmp`""
    $rc = $LASTEXITCODE
    if ($rc -eq 0) {
        cmd /c "`"$ae`" `"$name.aeo`" >nul 2> `"$name.tmp`""
        $rc = $LASTEXITCODE
    }
    $msg = if (Test-Path "$name.tmp") { [System.IO.File]::ReadAllText((Join-Path $errDir "$name.tmp"), $utf8) } else { '' }
    Remove-Item "$name.aeo","$name.tmp" -ErrorAction SilentlyContinue
    Pop-Location

    $want = if (Test-Path $expPath) { [System.IO.File]::ReadAllText($expPath, $utf8) } else { '' }
    if ($rc -eq 0) {
        $fail++; $failed += "$name (本应失败，却成功了)"
    } elseif ($msg -notlike "*$want*") {
        $fail++; $failed += "$name (错误信息不含 '$want'：$((Norm $msg) -replace "`n", ' '))"
    } else {
        $pass++
    }
}

Write-Host ""
Write-Host "══ Ae 回归结果 ══" -ForegroundColor Cyan
Write-Host ("  [+] 通过: {0}" -f $pass) -ForegroundColor Green
Write-Host ("  [-] 失败: {0}" -f $fail) -ForegroundColor $(if ($fail -eq 0) { 'Green' } else { 'Red' })
foreach ($f in $failed) { Write-Host "      [-] $f" -ForegroundColor Red }
Write-Host ""
if ($fail -gt 0) { exit 1 } else { exit 0 }
