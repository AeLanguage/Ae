# =============================================================================
#  控制台中文检查 —— Test/check_console.ps1
# -----------------------------------------------------------------------------
#  为什么单独一个脚本：控制台编码问题是"看不见的"——重定向到文件/管道时永远是对的，
#  只有【真控制台 + 非 UTF-8 代码页】才复现。run.ps1 里没法验证，所以这里：
#    1. 生成三个测试脚本（脚本输出 / 编译错误 / 手输中文）
#    2. 编译 consolecheck/conprobe.cpp
#    3. 探针自己开一个隐藏控制台 + 把代码页设成 936(GBK) + 跑子进程，
#       再读回【屏幕缓冲区里真正的字符】来判定
#    4. 打印报告，退出码 0 = 全通过
#
#  依赖：clang++（编译探针）、Apt/aec.exe、Apt/ae.exe。不需要管理员权限。
#  用法（在 Test 目录下）：powershell -ExecutionPolicy Bypass -File .\check_console.ps1
# =============================================================================
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$apt  = Join-Path (Split-Path -Parent $here) 'Apt'
$work = Join-Path $here 'consolecheck'
$utf8 = New-Object System.Text.UTF8Encoding($false)

foreach ($exe in @('aec.exe', 'ae.exe')) {
    if (-not (Test-Path (Join-Path $apt $exe))) { Write-Host "缺少工具链: $apt\$exe" -ForegroundColor Red; exit 2 }
}

# ── 1) 生成测试脚本（必须 UTF-8 无 BOM：aec 会校验源码编码）──
[System.IO.File]::WriteAllText((Join-Path $work 'con_test.ae'), @'
func main() {
    prln("中文输出测试：你好，世界")
    prln("ASCII ok")
    pr("不带换行的中文提示: ")
    prln("该换行了")
}
'@, $utf8)

[System.IO.File]::WriteAllText((Join-Path $work 'con_bad.ae'), @'
func main() {
    local a = }
}
'@, $utf8)

[System.IO.File]::WriteAllText((Join-Path $work 'con_in.ae'), @'
func main() {
    local s = inp("请输入: ")
    prln("收到 =", s)
}
'@, $utf8)

Write-Host "[*] 编译测试脚本..." -ForegroundColor Cyan
Push-Location $work
& (Join-Path $apt 'aec.exe') -q con_test.ae
if ($LASTEXITCODE -ne 0) { Pop-Location; Write-Host "[-] con_test.ae 编译失败" -ForegroundColor Red; exit 2 }
& (Join-Path $apt 'aec.exe') -q con_in.ae
if ($LASTEXITCODE -ne 0) { Pop-Location; Write-Host "[-] con_in.ae 编译失败" -ForegroundColor Red; exit 2 }
Pop-Location

# ── 2) 编译探针 ──
Write-Host "[*] 编译探针..." -ForegroundColor Cyan
Push-Location $work
clang++ -std=c++17 -O2 -Wall conprobe.cpp -o conprobe.exe -luser32 2>&1 | Select-String ' error' | Select-Object -First 6
if (-not (Test-Path conprobe.exe)) { Pop-Location; Write-Host "[-] 探针编译失败（需要 clang++）" -ForegroundColor Red; exit 2 }

# ── 3) 跑探针（它自己开隐藏控制台，代码页 936）──
$report = Join-Path $work 'conprobe_report.txt'
& .\conprobe.exe $report $apt $work
$rc = $LASTEXITCODE
Pop-Location

if (Test-Path $report) { Get-Content $report -Encoding UTF8 | Select-Object -First 20 }
Write-Host ""
if ($rc -eq 0) { Write-Host "[+] 控制台中文检查全部通过" -ForegroundColor Green; exit 0 }
Write-Host "[-] 控制台中文检查有失败项（完整报告：$report）" -ForegroundColor Red
exit 1
