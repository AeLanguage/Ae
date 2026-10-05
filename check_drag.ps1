# =============================================================================
#  拖动窗口时的动画检查 —— Test/check_drag.ps1
# -----------------------------------------------------------------------------
#  用户报的 bug：拖动窗口时所有动画都停了，松手才恢复。
#  原因是拖标题栏/拖边缘时 Windows 会在 DefWindowProc 里跑【自己的模态消息循环】，
#  脚本的 pump/run 循环被卡在里面，定时器没人派发。
#
#  这个脚本用真窗口 + 真消息复现：
#    ① 启动窗口程序（它每帧记一次时间，报出【最大帧间隔】）
#    ② 往窗口投 WM_SYSCOMMAND = SC_MOVE|HTCAPTION → 系统真的进入拖动模态循环
#    ③ 拖一会儿，再投 ESC / 鼠标消息让它结束
#    ④ 检查最大帧间隔：修好了就是几十毫秒；没修就是整段拖动时长（≥1 秒）
#
#  用法（在 Test 目录下）：powershell -ExecutionPolicy Bypass -File .\check_drag.ps1
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
& (Join-Path $apt 'aec.exe') -q dragrecv.ae
if ($LASTEXITCODE -ne 0) { Pop-Location; Write-Host "[-] dragrecv.ae 编译失败" -ForegroundColor Red; exit 2 }
Pop-Location

$out = Join-Path $work 'dragrecv.out.txt'
Remove-Item $out -ErrorAction SilentlyContinue
Write-Host "[*] 启动窗口程序..." -ForegroundColor Cyan
$p = Start-Process -FilePath (Join-Path $apt 'ae.exe') -ArgumentList (Join-Path $work 'dragrecv.aeo') `
                   -WorkingDirectory $work -RedirectStandardOutput $out -PassThru
Start-Sleep -Milliseconds 1200

Add-Type -Namespace DragChk -Name U -MemberDefinition @"
public delegate bool EnumProc(IntPtr h, IntPtr p);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint tid, ref GUITHREADINFO gti);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
[DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
[StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr extra; }
[StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }
[DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] arr, int size);
// ★ 用 SendInput（真输入注入）：SetCursorPos / mouse_event 那套，系统的移动循环反应不对
public static void MoveAbs(int x, int y) {
  INPUT[] a = new INPUT[1];
  a[0].type = 0;
  a[0].mi.dx = (int)((long)x * 65535L / (GetSystemMetrics(0) - 1));
  a[0].mi.dy = (int)((long)y * 65535L / (GetSystemMetrics(1) - 1));
  a[0].mi.dwFlags = 0x0001 | 0x8000;              // MOVE | ABSOLUTE
  SendInput(1, a, Marshal.SizeOf(typeof(INPUT)));
}
public static void Btn(uint flag) {
  INPUT[] a = new INPUT[1];
  a[0].type = 0;
  a[0].mi.dwFlags = flag;                          // LEFTDOWN=2 / LEFTUP=4
  SendInput(1, a, Marshal.SizeOf(typeof(INPUT)));
}
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int l, t, r, b; }
[StructLayout(LayoutKind.Sequential)] public struct GUITHREADINFO {
  public int cbSize; public int flags;
  public IntPtr hwndActive, hwndFocus, hwndCapture, hwndMenuOwner, hwndMoveSize, hwndCaret;
  public int rcLeft, rcTop, rcRight, rcBottom; }
"@

[void][DragChk.U]::SetProcessDPIAware()
$script:found = [IntPtr]::Zero
$cb = [DragChk.U+EnumProc]{
  param($h, $l)
  $sb = New-Object System.Text.StringBuilder 256
  [void][DragChk.U]::GetWindowTextW($h, $sb, 256)
  $p2 = 0
  [void][DragChk.U]::GetWindowThreadProcessId($h, [ref]$p2)
  if ($p2 -eq $p.Id -and $sb.ToString() -eq "DRAGRECV") { $script:found = $h }
  return $true
}
[void][DragChk.U]::EnumWindows($cb, [IntPtr]::Zero)
if ($script:found -eq [IntPtr]::Zero) {
  Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
  Write-Host "[-] 找不到测试窗口" -ForegroundColor Red; exit 2
}
$h = $script:found

# ── 拖动标题栏（库自己做拖动，不进系统模态循环）──
#   这里不注入真鼠标：直接投 WM_NCLBUTTONDOWN(HTCAPTION) + WM_MOUSEMOVE + WM_LBUTTONUP。
#   库的拖动逻辑就是用这两条消息里的坐标算位置，所以能完整验证"拖动期间动画照跑"。
Write-Host "[*] 拖动标题栏（投非客户区按下 + 一串移动 + 抬起）..." -ForegroundColor Cyan
[void][DragChk.U]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 400
$wr = New-Object DragChk.U+RECT
[void][DragChk.U]::GetWindowRect($h, [ref]$wr)
$pos0 = "{0},{1}" -f $wr.l, $wr.t
$capX = [int](($wr.r - $wr.l) / 2)      # 客户区坐标里的标题栏位置：y 用负数（标题栏在客户区上方）
$capY = -12
$scrX = $wr.l + [int](($wr.r - $wr.l) / 2)
$scrY = $wr.t + 12
$mk = { param($x, $y) [IntPtr](((($y -band 0xFFFF) -shl 16) -bor ($x -band 0xFFFF))) }
# 按下标题栏 → 库开始自己拖
[void][DragChk.U]::PostMessageW($h, 0x00A1, [IntPtr]2, (& $mk $scrX $scrY))   # WM_NCLBUTTONDOWN + HTCAPTION
Start-Sleep -Milliseconds 300

# 确认【没有】进系统模态循环（进去了脚本的循环就会被卡住）
$script:noModal = $false
$gti = New-Object DragChk.U+GUITHREADINFO
$gti.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($gti)
$script:pid2 = 0
$tid = [DragChk.U]::GetWindowThreadProcessId($h, [ref]$script:pid2)
if ([DragChk.U]::GetGUIThreadInfo($tid, [ref]$gti)) {
  $script:noModal = ($gti.hwndMoveSize -eq [IntPtr]::Zero)
}
if ($script:noModal) { Write-Host "[*] 没有进入系统模态循环（脚本的循环照跑）" -ForegroundColor Cyan }
else { Write-Host "[!] 进了系统模态循环" -ForegroundColor Yellow }

# ── 拖 1.6 秒：每 100ms 来一条鼠标移动（客户区坐标），窗口应该跟着走 ──
for ($i = 1; $i -le 16; $i++) {
  [void][DragChk.U]::PostMessageW($h, 0x0200, [IntPtr]::Zero, (& $mk ($capX + $i * 3) ($capY + $i * 1)))
  Start-Sleep -Milliseconds 100
}
[void][DragChk.U]::PostMessageW($h, 0x0202, [IntPtr]::Zero, (& $mk ($capX + 48) ($capY + 16)))  # WM_LBUTTONUP
Start-Sleep -Milliseconds 400
$wr2 = New-Object DragChk.U+RECT
[void][DragChk.U]::GetWindowRect($h, [ref]$wr2)
$script:moved = (($wr2.l -ne $wr.l) -or ($wr2.t -ne $wr.t))
Write-Host ("[*] 窗口位置 {0} → {1},{2}（真的动了 = {3}）" -f $pos0, $wr2.l, $wr2.t, $script:moved) -ForegroundColor Cyan
Write-Host "[*] 等脚本自己收尾..." -ForegroundColor Cyan
if (-not $p.WaitForExit(14000)) { $p.Kill() }
Start-Sleep -Milliseconds 300

$text = if (Test-Path $out) { [System.IO.File]::ReadAllText($out, $enc) } else { '' }
Write-Host "--- 脚本输出 ---"
Write-Host $text

$script:pass = 0; $script:fail = 0
function Check($name, $cond) {
  if ($cond) { Write-Host "[+] $name" -ForegroundColor Green; $script:pass++ }
  else       { Write-Host "[-] $name" -ForegroundColor Red;   $script:fail++ }
}

$m = [regex]::Match($text, '最大帧间隔 = (\d+) ms')
$gap = if ($m.Success) { [int]$m.Groups[1].Value } else { -1 }
$mf = [regex]::Match($text, '总帧数 = (\d+)')
$frames = if ($mf.Success) { [int]$mf.Groups[1].Value } else { 0 }

Check "脚本跑完并报了结果"            ($text -like '*最大帧间隔*')
Check "拖动没进系统模态循环（脚本循环没被卡住）" ($script:noModal)
Check "窗口真的被拖动了"              ($script:moved)
Check "动画一直在跑（帧数 > 400）"     ($frames -gt 400)
Check "拖动期间动画没停（最大间隔 < 200ms）" ($gap -ge 0 -and $gap -lt 200)
Write-Host ""
Write-Host ("测量值：总帧数 {0}，最大帧间隔 {1} ms" -f $frames, $gap)
if ($script:fail -eq 0) { Write-Host "[+] 拖动期间的动画检查全部通过" -ForegroundColor Green; exit 0 }
Write-Host "[-] 拖动期间的动画检查有失败项" -ForegroundColor Red; exit 1
