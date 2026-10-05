# =============================================================================
#  内置函数 run 的功能检查 —— Test/check_run.ps1
# -----------------------------------------------------------------------------
#  为什么单独一个脚本：run 的语义是"在一个程序里跑另一个 .aeo，跑完再回来"，
#  要验证的是一串【跨程序】的行为（顺序、隔离、参数、退出码、错误传播、
#  ui 收尾、嵌套上限），用 run.ps1 那种"单个 .ae + 期望输出"不好表达，
#  而且需要先把子程序编译好放在同一个目录里。
#
#  依赖：Apt 工具链（aec/ae/ui.dll）+ Windows。
#  用法（在 Test 目录下）：
#      powershell -ExecutionPolicy Bypass -File .\check_run.ps1
# =============================================================================
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$apt  = Join-Path (Split-Path -Parent $here) 'Apt'
$work = Join-Path $here 'runcheck'
$enc  = New-Object System.Text.UTF8Encoding($false)

foreach ($exe in @('aec.exe','ae.exe')) {
  if (-not (Test-Path (Join-Path $apt $exe))) { Write-Host "缺少工具链: $apt\$exe" -ForegroundColor Red; exit 2 }
}

Write-Host "[*] 编译子程序..." -ForegroundColor Cyan
Push-Location $work
$srcs = @('child2.ae','child.ae','uichild.ae','runmain.ae')   # 顺序无所谓，run 是运行期找 .aeo
foreach ($s in $srcs) {
  & (Join-Path $apt 'aec.exe') -q $s
  if ($LASTEXITCODE -ne 0) { Pop-Location; Write-Host "[-] $s 编译失败" -ForegroundColor Red; exit 2 }
}
Write-Host "[*] 运行主程序..." -ForegroundColor Cyan
$out = & (Join-Path $apt 'ae.exe') 'runmain.aeo' 2>&1
$code = $LASTEXITCODE
Pop-Location

$text = ($out -join "`n")
Write-Host "--- 程序输出 ---"
Write-Host $text
Write-Host "--- 退出码: $code ---"

$script:pass = 0; $script:fail = 0
function Check($name, $cond) {
  if ($cond) { Write-Host "[+] $name" -ForegroundColor Green; $script:pass++ }
  else       { Write-Host "[-] $name" -ForegroundColor Red;   $script:fail++ }
}
# 子串顺序：a 出现在 b 之前
function Before([string]$a, [string]$b) {
  $ia = $text.IndexOf($a); $ib = $text.IndexOf($b)
  return ($ia -ge 0 -and $ib -ge 0 -and $ia -lt $ib)
}
# ★ 断言一律用普通子串查找，别用 -like：-like 里 "[1]" 是通配字符类，会永远匹配不上
function Has([string]$needle) { return $text.Contains($needle) }

Check "程序正常跑完（退出码 0）"            ($code -eq 0)
Check "全部检查都跑到了"                    (Has '[done] 全部检查跑完')
Check "① 子程序跑完回到父程序（返回 0）"     (Has '[1] 子程序返回 = 0')
Check "② 子程序输出夹在父程序两条输出之间"   (Before '[0] 父程序开始' '[child] 开始' -and (Before '[child] 正常结束' '[1] 子程序返回'))
Check "③ 子程序有自己的全局（g=999）"        (Has '[child] 开始 参数个数= 0  我的全局 g= 999')
Check "③ 父程序的全局没被改到（g=7）"        (Has '[3] 父程序全局 g = 7')
Check "④ 参数传进子程序（os.args() 看得到）" (Has '[child] 开始 参数个数= 2')
Check "④ 带参数调用正常返回"                (Has '[4] 带参数返回 = 0')
Check "⑤ 子程序 exit(3) → run 返回 3"        (Has '[5] exit 码 = 3')
Check "⑥ 子程序里未捕获的错误可被父程序 catch" (Has '[6] 捕获 code = E2002')
Check "⑥ 错误消息里带子程序的位置"           (Has '[6] 消息里有子程序位置 = run("child.aeo") 里出错: 整数除以零')
Check "⑦ 找不到文件 → E0002"                 (Has '[7] 找不到文件 code = E0002')
Check "⑧ 三层嵌套（a → child → child2）"     (Has '[child2] 最内层也跑到了')
Check "⑧ 嵌套调用正常返回"                  (Has '[8] 嵌套返回 = 0')
Check "⑨ 子程序的窗口在它结束时被清掉"        (Has '[9] ui 前 count = 0' -and (Has '[9] ui 后 count = 0'))
Check "⑨ 子程序的定时器/回调不再触发"         (-not (Has '[uichild] 定时器（不该再出现）') -and -not (Has '[uichild] 被点了（不该再出现）'))
Check "⑨ 父程序自己的窗口回调照常工作"        (Has '[9] 父程序回调仍然工作 = true')
Check "⑩ 父程序开着窗口时跑子程序，窗口不受影响" (Has '[10] 跑子程序前 父窗口在 = true count = 1' -and (Has '[10] 跑完子程序 父窗口在 = true count = 1'))
Check "⑩ 父程序回调在子程序跑完后仍然工作"     (Has '[10] 父程序回调仍然工作 = true')
Check "⑩ 收尾后 count 回到 0"                (Has '[10] 收尾 count = 0')
Check "⑪ 自我递归被嵌套上限拦住（E2006）"     (Has '[11] 嵌套上限 code = E2006')
Check "⑪ 提示里说清是嵌套太深"               (Has '嵌套太深')

Write-Host ""
if ($script:fail -eq 0) { Write-Host "[+] run 检查全部通过（$script:pass 项）" -ForegroundColor Green; exit 0 }
Write-Host "[-] run 检查有 $script:fail 项失败" -ForegroundColor Red; exit 1
