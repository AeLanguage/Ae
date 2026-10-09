# Ae

**Ae 2.0.1b** —— 一门小巧但完整的动态脚本语言：语法干净、编译成字节码、自带系统库（`io` `os` `path`）。

三个标准库是**头文件实现**，编译期直接编进 `aec` / `ae`，不需要构建任何 `.so` / `.dll`。
函数是一等公民，闭包、传递、匿名、回调一样不少。

```ae
imp io

func main() {
    local cfg = {name = "Ae", ver = "2.0.1b"}
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
    return func() { n += 1  return n }
}
```

```console
$ aec hello.ae
[+] 编译成功: hello.aeo（1746 字节）
$ ae hello.aeo
你好，Ae 2.0.1b
计数: 11 12 13
和 = 14 | 个数 = 5
```

---

## 特性

**语言**

- 20 个关键字：`if else while do imp try catch for in break continue return local func and or not true false null`
- 6 种类型：`int`（64 位）`float` `bool` `null` `string` `table`，加上一等公民的 `func`
- 闭包、多返回值、复合赋值 `+= -= *= /=`
- 函数支持默认参数、变参，具名函数也能当值传递
- 字符串插值 `"v=${x}"`
- 表同时当数组和字典用：`{1, 2, name = "x"}`；`==` 比身份、`equal()` 比内容
- `.m` 模块用 `imp` 导入，写法和普通 Ae 代码一样，顶层变量也可导出
- 顶层语句先执行，然后自动调用 `main()`（如果有）

**工具链**

- 三个可执行文件：`aec` 编译器、`ae` 虚拟机、`aed` 反汇编器（跑脚本只要 `aec` + `ae`）
- 零第三方依赖，只要 C++17 标准库
- 编译产物是单个 `.aeo` 字节码文件（当前格式 `AeBc 1.8`）
- 诊断带错误码：源码摘录 + `^` 光标 + `help` + 运行期调用栈（`E0xxx` 工具链 / `E1xxx` 编译期 / `E2xxx` 运行期）

**库**

- 原生库 `io`（文件）、`os`（环境与时间）、`path`（路径）—— 头文件版，随 `aec` / `ae` 一起编译
- Ae 模块（`.m`，无需编译）：`json`、`args`、`math`
- 统一失败约定：**预期失败**返回 `null`/`false` + `<库>.lastError()`；**程序性错误**抛带栈的运行错误

**定位**

- 写小工具 / 批处理：一个 `.ae` 编译成一个 `.aeo`，有 `ae` 就能跑
- 教编程：关键字少、报错清楚
- 不做：面向对象体系

---

## 目录结构

```
Apt/               语言实现
├─ aec.cpp           编译器：词法/语法/类型检查 → AeBc 1.8 字节码
├─ ae.cpp            虚拟机：加载 .aeo、执行、诊断与调用栈
├─ aed.cpp           反汇编器：把 .aeo 打成可读的指令清单
├─ ae_builtin.h      内置库登记表：把头文件库注册给 aec / ae
├─ ae_console.h      Windows 控制台统一走 UTF-8
├─ ae_diag.h         诊断渲染：错误码 / 摘录 / 光标 / help / 栈
├─ ae_native.h       原生库 ABI（v4：脚本参数、读表、回调 Ae 函数）
├─ ae_libhost.h      宿主侧加载器（可选：动态库扩展）
├─ ae_path.h         路径的编码处理
├─ lib/              标准库
│  ├─ ae_lib_io.h  ae_lib_os.h  ae_lib_path.h   头文件版原生库
│  ├─ ae_libutil.h                   库共用工具
│  └─ args.m  json.m  math.m         用 Ae 自己写的模块
Test/                回归测试
├─ run.sh / run.ps1  跑 cases/ 与 errors/（默认 73 个用例全绿）
├─ cases/*.ae|*.out  正向用例
├─ errors/*.ae|*.err 失败用例
├─ check_run.ps1      跨程序自测：run() 的顺序 / 隔离 / 参数 / 退出码 / 错误传播
└─ check_console.ps1  控制台中文自测
build.ps1             Windows 一键构建
build.sh              Linux / macOS 构建
```

---

## 构建

需要 **C++17** 编译器，没有第三方依赖。

标准库 `io` / `os` / `path` 是头文件版（`Apt/lib/ae_lib_*.h`），由 `Apt/ae_builtin.h` 汇总后被
`aec.cpp` 与 `ae.cpp` `#include`，一起编进可执行文件——构建产物只有可执行文件，没有 `.so` / `.dll`。

### Windows

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

参数：`-Clean`（先删旧的）、`-OutDir <目录>`、`-Compiler g++`、`-Quiet`。默认就地构建到 `Apt\`。

手动：

```bat
clang++ -std=c++17 -O2 -Wall aec.cpp -o aec.exe
clang++ -std=c++17 -O2 -Wall ae.cpp  -o ae.exe
clang++ -std=c++17 -O2 -Wall aed.cpp -o aed.exe
```

### Linux / macOS

```bash
./build.sh                      # 三件套（--no-aed 只编 aec + ae）
OUT=/tmp/ae ./build.sh          # 构建到别处
CXX=clang++ ./build.sh          # 换编译器
./build.sh --clean
```

手动等价于：

```bash
g++ -std=c++17 -O2 -Wall aec.cpp -o aec
g++ -std=c++17 -O2 -Wall ae.cpp  -o ae
g++ -std=c++17 -O2 -Wall aed.cpp -o aed
```

本版主要在 Linux 上验证。Windows 的 `#ifdef` 分支与 `build.ps1` 仍在，但未做 CI 覆盖。

### 模块查找

内置库（`io` `os` `path`）在可执行文件里，`imp io` 直接用。

`.m` 模块按顺序找（`<exe>` = 可执行文件所在目录，`<script>` = 脚本所在目录）：

```
<exe>/lib  →  <exe>  →  <script>/lib  →  <script>  →  ./lib  →  ./  →  $AE_LIB_PATH
```

每个目录依次尝试 `<名字>.m`（Windows 上还看 `<名字>.dll`、`lib<名字>.dll`；Linux `lib<名字>.so`、`<名字>.so`；macOS `lib<名字>.dylib`、`<名字>.dylib`）。
找不到时（E1014）会列出试过的全部路径。

---

## 工具用法

```console
$ aec -h
Ae 编译器 (aec) 2.0.1b
用法: aec <源文件.ae> [-o 输出.aeo] [-q]
  -o <文件>        指定输出的字节码文件名（默认同名 .aeo）
  -q, --quiet      成功时不打印提示（只出错才输出）
  -h, --help       显示本帮助
输出字节码格式: AeBc 1.8
退出码: 0 成功  1 编译失败
```

```console
$ ae -h
Ae 虚拟机 (ae) 2.0.1b
用法: ae [--max-depth N] <file.aeo> [脚本参数...]
  --max-depth N, -m N   调用深度上限（默认 10000）
  -h, --help            显示帮助
  --version             显示版本
<file.aeo> 之后的参数原样传给脚本（os.args() / args 库可见）
退出码: 0 成功  2 运行错误  3 用法/IO 错误
```

```console
$ aed -h
Ae 反汇编器 (aed) 2.0.1b
用法: aed <字节码.aeo>
  把 .aeo 反汇编成人读的清单：头部信息 / 常量池 / 函数表 / 指令流 / 行号表
退出码: 0 成功  1 失败
```

```console
$ aec hello.ae              # 编译 → hello.aeo
$ ae hello.aeo 参数1 参数2   # 运行，后面的参数给脚本
$ aed hello.aeo             # 看字节码
```

---

## 语言速览

| 项 | 说明 |
|---|---|
| 注释 | `// 行注释`、`/* 块注释 */`（不嵌套） |
| 变量 | 赋值即创建全局：`x = 1`；函数里用 `local x = 1` |
| 判断 / 循环 | `if` `else` `while` `do-while`、`for i = 1, 5 { }`、`for k, v in t { }`；条件必须带括号 |
| 函数 | `func add(a, b) { return a + b }`、`func f(a, b = 2)`、`func f(a, rest...)`；具名函数可当值 |
| 真值 | 假：`false` `0` `""` `{}` `null` |
| 相等 | `==` 比身份（表/函数/字符串）/值（标量）；`equal(a, b)` 比内容 |
| 运算 | `+ - * / %`、比较、`and or not`、位运算、`**`；`/` 对整数是整除 |
| 字符串 | UTF-8，`len()` 按码点；索引从 1 开始；`+` 拼接；插值 `"v=${x}"`（单引号不插值） |
| 表 | `{1, 2, name = "x"}`；`len()` 数数组段、`count()` 数全部键；键只能是 int / string / bool |
| 错误 | `try { } catch (e) { }`，`e` 含 `code kind msg file line col stack` |
| 模块 | `imp mathx` → 找 `mathx.m`；`func` 与顶层变量都能用，`_` 开头不导出 |
| 标识符 | ASCII 字母/数字/下划线 |

---

## 标准库

| 库 | 形式 | 内容 | 失败方式 |
|---|---|---|---|
| `io` | 内置（头文件） | 读/写文件、逐行读、目录、文件句柄、字节 | `null`/`false` + `io.lastError()` |
| `os` | 内置（头文件） | 环境变量、工作目录、时间、随机、平台/PID/主机名、命令行参数 | `null`/`false` + `os.lastError()` |
| `path` | 内置（头文件） | 路径运算（统一用 `/`，`toNative()` 转回 `\`） | 基本不失败 |
| `json` | `.m` 模块 | `decode` `decodeFile` / `encode` `encodePretty` `encodeFile` | 解析失败抛 `E2008` |
| `args` | `.m` 模块 | 命令行参数解析（`parse` / `parseOrExit`、`usage` `intOr` `floatOr`） | 用法错误打印帮助并 `exit(3)` |
| `math` | `.m` 模块 | 69 个数学函数：三角 / 双曲 / 指数对数 / 幂 / 取整 / 组合 / 统计 / 随机 | 定义域错误返回 `nan`，不抛错 |

前三个内置，`imp io` 不需要任何外部文件。后三个是 `.m` 模块，放在 exe 旁的 `lib/` 或 `AE_LIB_PATH` 指到的目录里。

### math/json/args

用 Ae 自己写的（非 C++），417 行、69 个对外函数。
- 具体实现与使用方式请看官网

### 内置函数（不用 `imp`，36 个）

| 分类 | 函数 |
|---|---|
| 输入输出 | `pr` `prln` `inp` |
| 类型与转换 | `len` `str` `int` `float` `type` |
| 错误与控制 | `assert` `error` `exit` `raise` |
| 子程序 | `run`（跑另一个 .aeo，跑完再回来） |
| 字符串 | `substr` `ord` `chr` `find` `upper` `lower` `trim` `replace` `split` `join` `repeat` `format` |
| 表 | `delete` `insert` `pop` `append` `count` `copy` `deepcopy` `equal` |
| 内存回收 | `weak` `collect` `gcstats` |

表与闭包由 GC 统一管理，引用环（表环、递归闭包、混合）都会被回收。
`weak(t)` 把表变成弱值表，值不参与标记，条目只在别处还有强引用时保留；`weak(t, "n")` 取消。
`collect()` 同步完成一轮完整收集，返回回收数。`gcstats()` 返回统计表。

`args.m` / `json.m` 本身就是写模块的范例。

---

## 测试

```bash
cd Test
./run.sh                 # 全部
./run.sh -Filter p13     # 只跑名字含 p13 的
```

Windows 用 `powershell -ExecutionPolicy Bypass -File run.ps1`（参数同）。

- `cases/<名字>.ae` + `.out`：必须编译并成功运行，stdout 与 `.out` 一致
- `errors/<名字>.ae` + `.err`：必须失败，stderr 含 `.err` 的文本
- 加用例就是放一对文件进去，不用注册。当前规模：35 正向 + 38 失败 = **73 个用例**

跨程序 `run()` 与控制台中文有单独脚本：

```powershell
powershell -ExecutionPolicy Bypass -File check_run.ps1
powershell -ExecutionPolicy Bypass -File check_console.ps1
```

测试跑的是 `Apt/` 里的工具，改完源码要先构建。

---

## 字节码与反汇编

`.aeo` 开头是 `AeBc` 魔数 + 主/次版本号（当前 **1.8**），后面是常量池、函数表、指令流和行号表。

```console
$ aed hello.aeo
; ╔══════════════════════════════════════════════════════════╗
; ║  Ae Bytecode Disassembly  (hello.aeo)
; ╠══════════════════════════════════════════════════════════╣
; ║  Magic: AeBc  Version: 1.8
; ╚══════════════════════════════════════════════════════════╝

; ── Constant Pool (16 entries) ───────────────
  #  0  utf8   "Ae"
  #  1  utf8   "name"
  ...
```

加载时校验魔数、版本、常量池下标和函数体范围，坏文件报 `E2005`。

---

## 文档与官网

- 官网：<https://aelanguage.github.io/>
- 站点文档分三块：**教学文档**（12 章）、**库与函数**（12 章，每个函数有签名 + 说明 + 示例）、**报错**（5 章：编译期 E1xxx / 运行期 E2xxx / 库的报错与 lastError / 调试套路）
- 文档里的每个示例都真编译、真运行过

## 计划

- [ ] VM 引入 JIT（长远）
- [ ] AOT 编译器（长远）
- [ ] REPL（逐步推进中）
- [ ] 包管理（长远，需要生态）
- ~~OOP（遗弃）~~

## 许可证

<LICENSE>
