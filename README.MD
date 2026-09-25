# Ae

**Ae** 是一门小巧但完整的动态脚本语言：语法干净、编译成字节码、自带系统库和窗口/图形库（`ui`）。

它按“中文也不是二等公民”来设计：源码用 UTF-8、诊断里的中文算 2 列（光标不会错位）、Windows 控制台里不乱码，报错信息也全部是中文。

```ae
imp io

func main() {
    local cfg = {name = "Ae", ver = 1.7}
    prln("你好，" + cfg.name + " " + cfg.ver)

    local next = makeCounter(10)
    prln("计数:", next(), next(), next())

    local nums = {3, 1, 4, 1, 5}
    local sum = 0
    for i, v in nums { sum += v }
    prln("和 =", sum, "| 个数 =", len(nums))
}

func makeCounter(start) {
    local n = start
    return func() { n += 1  return n }     // 闭包
}
```

```console
$ aec hello.ae
[+] 编译成功: hello.aeo（1746 字节）
$ ae hello.aeo
你好，Ae 1.7
计数: 11 12 13
和 = 14 | 个数 = 5
```

---

## 特性一览

**语言**

- 19 个关键字：`if else while do imp try catch for in break continue return local func and or not true false null`
- 6 种类型：`int`（64 位）`float` `bool` `null` `string` `table`，加上一等公民的 `func`
- **闭包**（`func(x) { … }` 捕获外层变量）、`try/catch`、多返回值、复合赋值 `+= -= *= /=`
- 表（table）同时当数组和字典用：`{1, 2, name = "x"}`；空表是假值，`==` 比身份、`equal()` 比内容
- 模块：`.m` 文件用 `imp` 导入，写法和普通 Ae 代码一样
- 顶层语句先执行，然后自动调用 `main()`（如果有）

**工具链**

- 只有三个可执行文件：`aec` 编译器、`ae` 虚拟机、`aed` 反汇编器
- 零第三方依赖，只要 C++17 标准库；编译产物是单个 `.aeo` 字节码文件
- 带错误码的诊断：源码摘录 + `^` 光标 + `help` + 运行期调用栈（`E0xxx` 工具链 / `E1xxx` 编译期 / `E2xxx` 运行期）

**库**

- 原生库（C++ 写成 `.dll` / `.so`）：`io` 文件、`os` 环境与时间、`path` 路径、`ui` 窗口与图形（Windows / GDI+）
- Ae 模块：`json`、`args`
- 统一的失败约定：**预期失败**返回 `null`/`false` + `<库>.lastError()`；**程序性错误**抛带栈的运行错误

**适合用来做**

- 写小工具 / 批处理：一个 `.ae` 编译成一个 `.aeo`，有 `ae` 就能跑，装的东西只有三个可执行文件
- 教编程：关键字少、报错清楚、中文友好（中文只进字符串，标识符仍是 ASCII，不引入歧义）
- Windows 上的小桌面程序 / 画点东西：`ui` 能直接开窗口、画图形和文字、收鼠标键盘事件、定时器重绘
- 不打算做的：包管理、REPL、面向对象体系、协程 —— 需要这些时用别的语言更省事

---

## 目录结构

```
Ae/               语言实现（源码都在这儿）
├─ aec.cpp           编译器：词法/语法/类型检查 → AeBc 1.7 字节码
├─ ae.cpp            虚拟机：加载 .aeo、执行、诊断与调用栈
├─ aed.cpp           反汇编器：把 .aeo 打成可读的指令清单
├─ ae_console.h      Windows 控制台统一走 UTF-8（GBK 的 cmd 里中文不乱码）
├─ ae_diag.h         诊断渲染：错误码 / 摘录 / 光标 / help / 栈
├─ ae_native.h       原生库 ABI（当前 v4：脚本参数、读表、回调 Ae 函数）
├─ ae_libhost.h      宿主侧加载器：找库的目录顺序与候选文件名
└─ lib/              标准库（4 个原生库 + 2 个 Ae 模块）
│  ├─ io.cpp  os.cpp  path.cpp     跨平台原生库
│  ├─ ui.cpp                       Windows 专用（GDI+），近 4000 行
│  ├─ ae_libutil.h                 库共用工具：参数检查 / lastError
│  ├─ args.m  json.m               用 Ae 自己写的模块（不用编译）
│  └─ *.dll / *.so                 构建产物（放这里就能被 imp 找到）
├─ Test/                回归测试
│  ├─ run.ps1           跑 cases/ 与 errors/（默认 70 个用例全绿）
│  ├─ cases/*.ae|*.out  正向用例：必须编译并成功运行，输出与 .out 一致
│  ├─ errors/*.ae|*.err 失败用例：必须失败，stderr 含 .err 里的文本
│  └─ check_*.ps1       交互类自测：ui 事件、拖拽、鼠标、控制台中文
├─ build.ps1            Windows 一键构建（PowerShell）
├─ build.sh             Linux / macOS 构建
└─ build/               临时产物（可有可无，不进版本库）
```

---

## 构建

需要 **C++17** 编译器（Windows 上推荐 `clang++`，也可以用 `g++` 或 MSVC 的 `cl`）。没有第三方依赖。

### Windows：一键构建

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

它会编译 **7 个东西**（3 个工具 + 4 个原生库），并把 `args.m`、`json.m` 拷到 `lib\`：

| 源文件 | 产物 | 链接参数 |
|---|---|---|
| `aec.cpp` | `aec.exe` | — |
| `ae.cpp` | `ae.exe` | — |
| `aed.cpp` | `aed.exe` | — |
| `lib\io.cpp` | `lib\io.dll` | — |
| `lib\os.cpp` | `lib\os.dll` | — |
| `lib\path.cpp` | `lib\path.dll` | — |
| `lib\ui.cpp` | `lib\ui.dll` | `-luser32 -lgdi32 -lgdiplus -lwinmm` |

脚本参数：

```powershell
build.ps1 -Clean                  # 先删旧的 exe/dll 再编
build.ps1 -OutDir D:\tmp\ae       # 构建到别处（工具在 D:\tmp\ae，库在 D:\tmp\ae\lib）
build.ps1 -Compiler g++           # 指定编译器（默认 clang++ → g++ → cl）
build.ps1 -Quiet                  # 只报错，不打印编译器告警
```

默认**就地**构建到 `Apt\` 与 `Apt\lib\` —— `Test\run.ps1` 就是从这里取工具的。

### Windows：手动敲（想知道每一步在干什么）

在 `Apt` 目录下：

```bat
clang++ -std=c++17 -O2 -Wall aec.cpp -o aec.exe
clang++ -std=c++17 -O2 -Wall ae.cpp  -o ae.exe
clang++ -std=c++17 -O2 -Wall aed.cpp -o aed.exe

cd lib
clang++ -std=c++17 -O2 -Wall -shared io.cpp   -o io.dll
clang++ -std=c++17 -O2 -Wall -shared os.cpp   -o os.dll
clang++ -std=c++17 -O2 -Wall -shared path.cpp -o path.dll
clang++ -std=c++17 -O2 -Wall -shared ui.cpp   -o ui.dll -luser32 -lgdi32 -lgdiplus -lwinmm
```

编译时出现的两条告警是历史遗留（`io.cpp` 的 `&&` 优先级提示、`os.cpp` 里一个未用到的静态函数），不影响功能。

### Linux / macOS

```bash
./build.sh                      # 三件套 + libio.so / libos.so / libpath.so（macOS 上是 .dylib）
OUT=/tmp/ae ./build.sh          # 构建到别处
CXX=clang++ ./build.sh          # 换编译器
```

手动等价于：

```bash
g++ -std=c++17 -O2 -Wall aec.cpp -o aec
g++ -std=c++17 -O2 -Wall ae.cpp  -o ae
g++ -std=c++17 -O2 -Wall aed.cpp -o aed
g++ -std=c++17 -O2 -Wall -shared -fPIC lib/io.cpp   -o lib/libio.so
g++ -std=c++17 -O2 -Wall -shared -fPIC lib/os.cpp   -o lib/libos.so
g++ -std=c++17 -O2 -Wall -shared -fPIC lib/path.cpp -o lib/libpath.so
```

> `ui` 用的是 Windows 的 GDI+，**只能在 Windows 上编译**；Linux/macOS 上其余部分都能跑。
> `args.m`、`json.m` 是 Ae 写的模块，不需要编译，拷到 `lib/` 即可。
>
> 说明：本项目主要在 Windows 上开发和测试，Linux / macOS 的代码路径已经写好（`#ifdef` 分支与 `build.sh`），但还没有 CI 覆盖；遇到问题按“手动命令”逐条敲一遍通常就能定位。

### 库放哪：`imp` 怎么找库

`ae` 按顺序在这些目录里找（`<exe>` = 可执行文件所在目录，`<script>` = 脚本所在目录）：

```
<exe>/lib  →  <exe>  →  <script>/lib  →  <script>  →  ./lib  →  ./  →  $AE_LIB_PATH
```

每个目录里依次尝试这些文件名：

- Windows：`<名字>.dll`、`lib<名字>.dll`（再找 `<名字>.m`）
- Linux：`lib<名字>.so`、`<名字>.so`；macOS：`lib<名字>.dylib`、`<名字>.dylib`（也都再找 `<名字>.m`）

所以最省事的做法就是**把 dll 放在 exe 旁边的 `lib\`**；想放到别处就设环境变量 `AE_LIB_PATH`。
找不到库时会报 `E1014`，并把试过的路径全列出来。

### 只改了一个库 / 只改了一个工具

不用全量重编，直接编需要的那一个就行：

```powershell
cd Apt\lib
clang++ -std=c++17 -O2 -Wall -shared ui.cpp -o ui.dll -luser32 -lgdi32 -lgdiplus -lwinmm
```

（`ui` 的图形改动只影响 `ui.dll`，`ae.exe` 不用重编。）

### 构建/使用中的几个坑

| 现象 | 原因与处理 |
|---|---|
| `.ps1` 脚本报奇怪的语法错误（中文全变成 `缂栬瘧` 之类） | 脚本被存成了 **不带 BOM 的 UTF-8**，PowerShell 5.1 按 GBK 读。存回 **UTF-8 with BOM** 即可（文件头三字节应是 `EF BB BF`） |
| 构建脚本里 `clang++ ... 2>&1 \| ...` 之后整个脚本停住 | PS 5.1 把原生程序的 stderr 当 ErrorRecord，配合 `$ErrorActionPreference='Stop'` 会把一条普通告警变成终止性错误。`build.ps1` 用 `cmd /c ... > 日志 2>&1` 绕开了这个问题 |
| 链接时报 `cannot open output file ...dll: Permission denied` | 那个 dll 正被某个 `ae.exe` 进程占用，关掉窗口/进程再编（或先删掉它） |
| `imp ui` 报找不到库 | `ui.dll` 不在 `ae.exe` 旁边的 `lib\` 里；确认构建产物位置，或设 `AE_LIB_PATH` |
| 改了 `ae_native.h` 的 ABI 后老库加载失败 | 原生库要报 `abiVersion`，对不上会 `E1015`；重新编译对应的库 |

---

## 三件套用法

```console
$ aec -h
Ae 编译器 (aec)
用法: aec <源文件.ae> [-o 输出.aeo] [-q]
  -o <文件>        指定输出的字节码文件名（默认同名 .aeo）
  -q, --quiet      成功时不打印提示（只出错才输出）
  -h, --help       显示本帮助
输出字节码格式: AeBc 1.7
退出码: 0 成功  1 编译失败
提示: 用法里第二个位置参数也可以直接写输出文件（aec a.ae b.aeo）
```

```console
$ ae -h
Ae 虚拟机 (ae) 1.1
用法: ae [--max-depth N] <file.aeo> [脚本参数...]
  --max-depth N, -m N   调用深度上限（默认 10000）
  -h, --help            显示帮助
  --version             显示版本
<file.aeo> 之后的参数原样传给脚本（os.args() / args 库可见）
退出码: 0 成功  2 运行错误  3 用法/IO 错误
```

```console
$ aed -h
Ae 反汇编器 (aed)
用法: aed <字节码.aeo>
  把 .aeo 反汇编成人读的清单：头部信息 / 常量池 / 函数表 / 指令流 / 行号表
  字节码用 aec 从 .ae 编译得到（aec hello.ae → hello.aeo）
退出码: 0 成功  1 失败
```

典型流程：

```console
$ aec hello.ae            # 编译 → hello.aeo
$ ae hello.aeo 参数1 参数2  # 运行，后面的参数给脚本
$ aed hello.aeo           # 想看字节码长什么样
```

---

## 语言速览

| 项 | 说明 |
|---|---|
| 注释 | `// 行注释`、`/* 块注释 */`（不嵌套） |
| 变量 | 赋值即创建全局：`x = 1`；函数里用 `local x = 1` |
| 判断 / 循环 | `if (cond) { } else { }`、`while (c) { }`、`do { } while (c)`、`for i = 1, 5 { }`、`for i = 5, 1, -1 { }`、`for k, v in t { }` —— 条件必须带括号 |
| 函数 | `func add(a, b) { return a + b }`；匿名函数 `func(x) { … }`；多返回值 `return 1, 2` + `local a, b = f()` |
| 真值 | 假：`false` `0` `""` `{}` `null`，其余为真 |
| 相等 | `==` 比身份（表/函数）/值（标量）；`equal(a, b)` 比表内容；**`1 == 1.0` 会报错**，要显式统一类型 |
| 运算 | `+ - * / %`（`/` 是整数除法：`7 / 2` = `3`，`7.0 / 2` = `3.5`）、比较、`and or not`、位运算；除数为 0 一律报错 |
| 字符串 | UTF-8，`len()` 按码点算；索引从 **1** 开始；`+` 拼接（数字也能直接拼） |
| 表 | `{1, 2, name = "x"}`；数组段从 1 连续，`len()` 数数组段、`count()` 数全部键；键只能是 int/string |
| 错误 | `try { } catch (e) { }`，`e` 里有 `code kind msg file line col stack`；`error()` / `assert()` / `exit(n)` |
| 模块 | `imp mathx` → 找 `mathx.m`；模块里定义的 `func` 都能用 |
| 标识符 | 只能用 ASCII 字母/数字/下划线 —— **中文只能写在字符串和注释里** |

几个容易踩的点（写完第一版代码基本都会遇到）：

- `1 == 1.0` → `E2001 不能直接比较 int 与 float`；写 `float(x)` 或 `1.0`。
- 具名函数**不能当值传递**（`apply(dbl, 3)` 会报未声明变量）；要传就用 `func(x) { return dbl(x) }` 包一层，或直接写匿名函数。
- `func(x) { … }` 这种匿名函数只能写在**函数体里面**，顶层不行。
- 索引、`substr` 起点都从 1 开始。
- 库函数的**参数个数**是编译期检查的（`E1011`），参数类型是运行期检查的（`E2009`）。

---

## 标准库

| 库 | 形式 | 内容 | 失败方式 |
|---|---|---|---|
| `io` | 原生 | 读/写文件、逐行读、目录、文件句柄、字节 | `null`/`false` + `io.lastError()` |
| `os` | 原生 | 环境变量、工作目录、时间、随机、平台/PID/主机名、命令行参数 | `null`/`false` + `os.lastError()` |
| `path` | 原生 | 纯字符串路径运算（统一用 `/`，`toNative()` 转回 `\`） | 基本不失败 |
| `ui` | 原生 | 窗口、消息循环、图形（矩形/圆/路径/渐变/虚线）、文字、图片、画布、组、事件、定时器、脏矩形重绘 | 建对象失败 → `null` + `ui.lastError()`；参数/取值错 → 抛 `E2009` |
| `json` | Ae 模块 | `decode` `decodeFile` / `encode` `encodePretty` `encodeFile` | 解析失败直接抛 `E2008` |
| `args` | Ae 模块 | 命令行参数解析（`parse` / `parseOrExit`，另有 `usage` `intOr` `floatOr`） | 用法错误可自动打印帮助并 `exit(3)` |

内置函数（不用 `imp`，共 32 个）：

| 分类 | 函数 |
|---|---|
| 输入输出 | `pr` `prln` `inp` |
| 类型与转换 | `len` `str` `int` `float` `type` |
| 错误与控制 | `assert` `error` `exit` `raise` |
| 字符串 | `substr` `ord` `chr` `find` `upper` `lower` `trim` `replace` `split` `join` `repeat` `format` |
| 表 | `delete` `insert` `pop` `append` `count` `copy` `deepcopy` `equal` |

`args.m` / `json.m` 本身就是写模块的范例，可以直接当参考。

---

## 测试

```powershell
cd Test
powershell -ExecutionPolicy Bypass -File run.ps1              # 全部
powershell -ExecutionPolicy Bypass -File run.ps1 -Filter p13  # 只跑名字含 p13 的
```

约定：

- `cases/<名字>.ae` + `cases/<名字>.out`：必须**编译并成功运行**，stdout 与 `.out` 完全一致。
- `errors/<名字>.ae` + `errors/<名字>.err`：必须**失败**（编译期或运行期都行），stderr 里要包含 `.err` 的文本。
- 加用例就是放一对文件进去，不用注册。当前规模：33 个正向 + 37 个失败 = **70 个用例**。

交互类的东西（窗口、事件、拖拽、控制台中文）用单独的脚本测：

```powershell
powershell -ExecutionPolicy Bypass -File check_events.ps1     # 点击/移动/进出/键盘/关窗回调
powershell -ExecutionPolicy Bypass -File check_drag.ps1       # 标题栏拖动，且拖动期间动画不停
powershell -ExecutionPolicy Bypass -File check_mouse.ps1      # 命中测试、鼠标形状、帧率不掉
powershell -ExecutionPolicy Bypass -File check_console.ps1    # GBK 控制台里的中文输出
```

这几个脚本会**真的创建窗口并投递鼠标/键盘消息**（屏幕上会闪一下窗口），需要有桌面会话；无头环境里跑不了。

> `run.ps1` 跑的是 `Apt/` 里的工具，所以**改完 `.cpp` 要先构建**，否则测的还是旧版本。

---

## 字节码与反汇编

`.aeo` 文件开头是 `AeBc` 魔数 + 主/次版本号（当前 **1.7**，闭包版本），后面是常量池、函数表、指令流和行号表。
`aed` 能把它们打出来：

```console
$ aed hello.aeo
; ╔══════════════════════════════════════════════════════════╗
; ║  Ae Bytecode Disassembly  (hello.aeo)
; ╠══════════════════════════════════════════════════════════╣
; ║  Magic: AeBc  Version: 1.7
; ╚══════════════════════════════════════════════════════════╝

; ── Constant Pool (16 entries) ───────────────
  #  0  utf8   "Ae"
  #  1  utf8   "name"
  #  2  double 1.7
  #  3  utf8   "ver"
  #  4  utf8   "你好，"
  ...
```

虚拟机在加载时会校验魔数、版本、常量池下标和函数体范围，坏文件报 `E2005`（不会拿着坏字节码乱跑）。

---

## 文档与官网

- 官网源码仓库：<https://github.com/AeLanguage/aelanguage.github.io>
- 站点文档分三大块：
  **教学文档**（12 章，从“把 Ae 跑起来”到综合练习）、
  **库与函数**（12 章，每个函数都有签名 + 说明 + 小示例）、
  **报错**（5 章：怎么读一条报错 / 编译期 E1xxx / 运行期 E2xxx / 库的报错与 lastError / 处理与调试的套路）。
- 文档里的每个示例都真编译、真运行过（示例脚本与官网源码放在一起维护）。

## 许可证
<LICENSE>
