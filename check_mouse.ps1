# =============================================================================
#  鼠标移动时的帧率检查 —— Test/check_mouse.ps1
# -----------------------------------------------------------------------------
#  用户报的 bug①：鼠标在窗口里移动时，所有动画掉到 30fps 上下。
#  原因是命中测试对每个图形都真的建一遍路径（800 个图形 × 一条 mousemove = 31ms）。
#  修好之后：灌鼠标的那一段和没灌的那一段，帧数应该差不多。
#
#  做法：跑一个"800 个图形 + 动画"的窗口程序，它每 500ms 报一次累计帧数；
#        PowerShell 在中间 1.5 秒里往窗口猛投 WM_MOUSEMOVE，最后比较各段的帧数。
#
#  用法（在 Test 目录下）：powershell -ExecutionPolicy Bypass -File .\check_mouse.ps1
# =============================================================================
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$apt  = Join-Path (Split-Path -Parent $here) 'Apt'
$work = Join-Path $here 'eventscheck'
$enc  = New-Object System.Text.UTF8Encoding($false)

Write-Host "[*] 编译验收脚本..." -ForegroundColor Cyan
Push-Location $work
& (Join-Path $apt 'aec.exe') -q mouseflood.ae
if ($LASTEXITCODE -ne 0) { Pop-Location; Write-Host "[-] mouseflood.ae 编译失败" -ForegroundColor Red; exit 2 }
Pop-Location

$out = Join-Path $work 'mouseflood.out.txt'
Remove-Item $out -ErrorAction SilentlyContinue
Write-Host "[*] 启动窗口程序..." -ForegroundColor Cyan
$p = Start-Process -FilePath (Join-Path $apt 'ae.exe') -ArgumentList (Join-Path $work 'mouseflood.aeo') `
                   -WorkingDirectory $work -RedirectStandardOutput $out -PassThru
Start-Sleep -Milliseconds 1200

Add-Type -Namespace MsChk -Name U -MemberDefinition @"
public delegate bool EnumProc(IntPtr h, IntPtr p);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
"@

[void][MsChk.U]::SetProcessDPIAware()
$script:found = [IntPtr]::Zero
$cb = [MsChk.U+EnumProc]{
  param($h, $l)
  $sb = New-Object System.Text.StringBuilder 256
  [void][MsChk.U]::GetWindowTextW($h, $sb, 256)
  $p2 = 0
  [void][MsChk.U]::GetWindowThreadProcessId($h, [ref]$p2)
  if ($p2 -eq $p.Id -and $sb.ToString() -eq "MOUSEFLOOD") { $script:found = $h }
  return $true
}
[void][MsChk.U]::EnumWindows($cb, [IntPtr]::Zero)
if ($script:found -eq [IntPtr]::Zero) {
  Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
  Write-Host "[-] 找不到测试窗口" -ForegroundColor Red; exit 2
}
$h = $script:found
$mk = { param($x, $y) [IntPtr](((($y -band 0xFFFF) -shl 16) -bor ($x -band 0xFFFF))) }

# ── 灌鼠标 1.5 秒：约 150 条 WM_MOUSEMOVE（比真鼠标的 125Hz 还密一点）──
Write-Host "[*] 往窗口猛投 WM_MOUSEMOVE（约 150 条 / 1.5 秒）..." -ForegroundColor Cyan
for ($i = 0; $i -lt 150; $i++) {
  $x = 320 + ($i % 20)
  $y = 200 + ($i % 15)
  [void][MsChk.U]::PostMessageW($h, 0x0200, [IntPtr]::Zero, (& $mk $x $y))
  Start-Sleep -Milliseconds 10
}
Write-Host "[*] 灌完了，等脚本自己收尾..." -ForegroundColor Cyan
if (-not $p.WaitForExit(12000)) { $p.Kill() }
Start-Sleep -Milliseconds 300

$text = if (Test-Path $out) { [System.IO.File]::ReadAllText($out, $enc) } else { '' }
Write-Host "--- 脚本输出 ---"
Write-Host $text

# 解析每 500ms 的累计帧数，算每一段的帧数
$rows = @()
foreach ($line in ($text -split "`n")) {
  $m = [regex]::Match($line, '\[帧\] (\d+) (\d+)')
  if ($m.Success) { $rows += ,@([int]$m.Groups[1].Value, [int]$m.Groups[2].Value) }
}
$seg = @()
for ($i = 1; $i -lt $rows.Count; $i++) { $seg += ($rows[$i][1] - $rows[$i-1][1]) }
Write-Host ("[*] 每 500ms 的帧数：{0}" -f ($seg -join ', ')) -ForegroundColor Cyan

$mf = [regex]::Match($text, '总帧数 = (\d+)')
$frames = if ($mf.Success) { [int]$mf.Groups[1].Value } else { 0 }
$mg = [regex]::Match($text, '最大帧间隔 = (\d+) ms')
$gap = if ($mg.Success) { [int]$mg.Groups[1].Value } else { -1 }

$script:pass = 0; $script:fail = 0
function Check($name, $cond) {
  if ($cond) { Write-Host "[+] $name" -ForegroundColor Green; $script:pass++ }
  else       { Write-Host "[-] $name" -ForegroundColor Red;   $script:fail++ }
}

# 灌鼠标那段（第 3~5 段，即 1.0~2.5 秒）和前面的基线比
$base = if ($seg.Count -ge 2) { $seg[1] } else { 0 }
$flood = @()
if ($seg.Count -ge 5) { $flood = $seg[2..4] } elseif ($seg.Count -ge 3) { $flood = $seg[2..($seg.Count-1)] }
$floodMin = if ($flood.Count -gt 0) { ($flood | Measure-Object -Minimum).Minimum } else { 0 }
$floodAvg = if ($flood.Count -gt 0) { [int](($flood | Measure-Object -Average).Average) } else { 0 }

Check "脚本跑完并报了结果"               ($text -like '*最大帧间隔*')
Check "动画一直在跑（总帧数 > 1000）"     ($frames -gt 1000)
Check "灌鼠标那段没有掉速（每 500ms 至少 100 帧）" ($floodMin -ge 100)
if ($base -gt 0) {
  Check "灌鼠标那段不低于基线的 60%"      ($floodAvg -ge [int]($base * 0.6))
}
Check "全程没有长卡顿（最大帧间隔 < 200ms）" ($gap -ge 0 -and $gap -lt 200)
Write-Host ""
Write-Host ("测量值：总帧数 {0}，最大帧间隔 {1} ms，基线 {2} 帧/500ms，灌鼠标段 {3}（最低 {4}）" -f $frames, $gap, $base, $floodAvg, $floodMin)
if ($script:fail -eq 0) { Write-Host "[+] 鼠标移动时的帧率检查全部通过" -ForegroundColor Green; exit 0 }
Write-Host "[-] 鼠标移动时的帧率检查有失败项" -ForegroundColor Red; exit 1
