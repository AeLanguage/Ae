# =============================================================================
#  真实窗口消息检查 —— Test/check_events.ps1
# -----------------------------------------------------------------------------
#  为什么单独一个脚本：事件有两条路径 ——
#    ① ui.send(...) 注入        ← run.ps1 的用例覆盖这条
#    ② 真实窗口消息 → WndProc → 队列 → pump 派发   ← 用户真正在用的那条
#  第 ② 条曾经有个只有真消息才暴露的 bug：Windows 只给 WM_LBUTTONDOWN/UP，
#  【不给 click】—— "点击"必须自己合成。只测注入路径的话测试全绿、用户点按钮没反应。
#
#  这里往真窗口 PostMessage 真消息（含"按下后拖远再抬起不应算点击"），再检查脚本输出。
#  依赖：Apt 工具链 + Windows。不需要管理员权限。
#  用法（在 Test 目录下）：powershell -ExecutionPolicy Bypass -File .\check_events.ps1
# =============================================================================
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$apt  = Join-Path (Split-Path -Parent $here) 'Apt'
$work = Join-Path $here 'eventscheck'
$enc  = New-Object System.Text.UTF8Encoding($false)

foreach ($exe in @('aec.exe','ae.exe')) {
  if (-not (Test-Path (Join-Path $apt $exe))) { Write-Host "缺少工具链: $apt\$exe" -ForegroundColor Red; exit 2 }
}

Write-Host "[*] 编译验收脚本..." -ForegroundColor Cyan
Push-Location $work
& (Join-Path $apt 'aec.exe') -q clickrecv.ae
if ($LASTEXITCODE -ne 0) { Pop-Location; Write-Host "[-] clickrecv.ae 编译失败" -ForegroundColor Red; exit 2 }
Pop-Location

$out = Join-Path $work 'clickrecv.out.txt'
Remove-Item $out -ErrorAction SilentlyContinue
Write-Host "[*] 启动窗口程序..." -ForegroundColor Cyan
$p = Start-Process -FilePath (Join-Path $apt 'ae.exe') -ArgumentList (Join-Path $work 'clickrecv.aeo') `
                   -WorkingDirectory $work -RedirectStandardOutput $out -PassThru
Start-Sleep -Milliseconds 1600

Add-Type -Namespace EvChk -Name U -MemberDefinition @"
public delegate bool EnumProc(IntPtr h, IntPtr p);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
"@

[void][EvChk.U]::SetProcessDPIAware()   # ★ 必须在查询/投递坐标之前：PowerShell 默认不感知 DPI，
                                        #   而 Ae 是感知的，坐标会被系统按缩放比换算（120 DPI ×1.25）
$script:found = [IntPtr]::Zero
$cb = [EvChk.U+EnumProc]{
  param($h, $l)
  $sb = New-Object System.Text.StringBuilder 256
  [void][EvChk.U]::GetWindowTextW($h, $sb, 256)
  $p2 = 0
  [void][EvChk.U]::GetWindowThreadProcessId($h, [ref]$p2)
  if ($p2 -eq $p.Id -and $sb.ToString() -eq "CLICKRECV") { $script:found = $h }
  return $true
}
[void][EvChk.U]::EnumWindows($cb, [IntPtr]::Zero)
if ($script:found -eq [IntPtr]::Zero) {
  Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
  Write-Host "[-] 找不到测试窗口" -ForegroundColor Red; exit 2
}

$mk = { param($x, $y) [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF)) }
$h  = $script:found

# ① 按钮正中（那里盖着文字标签，顺便验证穿透）
[void][EvChk.U]::PostMessageW($h, 0x0200, [IntPtr]::Zero, (& $mk 100 50))   # WM_MOUSEMOVE
Start-Sleep -Milliseconds 200
[void][EvChk.U]::PostMessageW($h, 0x0201, [IntPtr]1, (& $mk 100 50))        # WM_LBUTTONDOWN
Start-Sleep -Milliseconds 120
[void][EvChk.U]::PostMessageW($h, 0x0202, [IntPtr]::Zero, (& $mk 100 50))   # WM_LBUTTONUP
Start-Sleep -Milliseconds 250
# ② 按钮左下角（避开文字标签）
[void][EvChk.U]::PostMessageW($h, 0x0200, [IntPtr]::Zero, (& $mk 40 70))
Start-Sleep -Milliseconds 120
[void][EvChk.U]::PostMessageW($h, 0x0201, [IntPtr]1, (& $mk 40 70))
Start-Sleep -Milliseconds 100
[void][EvChk.U]::PostMessageW($h, 0x0202, [IntPtr]::Zero, (& $mk 40 70))
Start-Sleep -Milliseconds 250
# ③ 按下后拖很远再抬起 → 不应算点击
[void][EvChk.U]::PostMessageW($h, 0x0201, [IntPtr]1, (& $mk 30 30))
Start-Sleep -Milliseconds 100
[void][EvChk.U]::PostMessageW($h, 0x0202, [IntPtr]::Zero, (& $mk 200 140))
Start-Sleep -Milliseconds 250
# ③ 键盘
[void][EvChk.U]::PostMessageW($h, 0x0100, [IntPtr]65, [IntPtr]::Zero)       # WM_KEYDOWN 'A'
Start-Sleep -Milliseconds 250
# ④ 点 X 关窗（走 close 回调）
[void][EvChk.U]::PostMessageW($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CLOSE
Start-Sleep -Milliseconds 900
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

$text = if (Test-Path $out) { [System.IO.File]::ReadAllText($out, $enc) } else { '' }
Write-Host "--- 脚本输出 ---"
Write-Host $text

$script:pass = 0; $script:fail = 0
function Check($name, $cond) {
  if ($cond) { Write-Host "[+] $name" -ForegroundColor Green; $script:pass++ }
  else       { Write-Host "[-] $name" -ForegroundColor Red;   $script:fail++ }
}
Check "真鼠标点击产生 click 事件（2 次）"   ($text -like '*共收到 click 2 次*')
Check "事件坐标正确（正中 100,50）"           ($text -like '*click x=100 y=50*')
Check "事件坐标正确（左下角 40,70）"          ($text -like '*click x=40 y=70*')
Check "事件目标是按钮（穿透了文字标签）"   ($text -like '*target是按钮=true*')
Check "拖动后抬起不算点击（4px 容差）"     (-not ($text -like '*x=200*'))
Check "mouseenter 触发"                   ($text -like '*mouseenter*')
Check "keydown 触发"                      ($text -like '*keydown key=65*')
Check "close 回调跑了且放行关窗"           ($text -like '*退出*')
Write-Host ""
if ($script:fail -eq 0) { Write-Host "[+] 真实消息检查全部通过" -ForegroundColor Green; exit 0 }
Write-Host "[-] 真实消息检查有失败项" -ForegroundColor Red; exit 1
