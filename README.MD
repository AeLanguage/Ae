# Ae

一门小巧的动态脚本语言：语法干净、编译成字节码、自带系统库。函数是一等公民，闭包、传递、匿名、回调一样不少。

```ae
imp io

func main() {
    local cfg = {name = "Ae", ver = "2.1.0"}
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
[+] 编译成功: hello.aeo
$ ae hello.aeo
你好，Ae 2.1.0
计数: 11 12 13
和 = 14 | 个数 = 5
```

三个可执行文件：`aec` 编译器、`ae` 虚拟机、`aed` 反汇编器。零第三方依赖，只要 C++17。

- 语言：20 个关键字，6 种类型（`int` `float` `bool` `null` `string` `table`），闭包、多返回值、默认参数、变参、字符串插值、切片、推导、`try/catch/finally`、`defer`
- 库：`io` `os` `path` 内置；`json` `args` `math` 是 `.m` 模块
- 诊断：错误码 + 源码摘录 + `^` 光标 + 运行期调用栈（`E0xxx` 工具链 / `E1xxx` 编译期 / `E2xxx` 运行期）

官网与完整文档：<https://aelanguage.github.io/>

---

## 构建

```bash
./build.sh                  # Linux / macOS / Termux
```

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1    # Windows
```

产物在 `Apt/`。手动编译等价：

```bash
g++ -std=c++17 -O2 -Wall Apt/aec.cpp -o Apt/aec
g++ -std=c++17 -O2 -Wall Apt/ae.cpp Apt/ae_main.cpp -o Apt/ae
g++ -std=c++17 -O2 -Wall Apt/aed.cpp -o Apt/aed
```

常用参数：`--clean` / `-Clean`、`--no-aed`、`OUT=/tmp/ae` / `-OutDir`、`CXX=clang++` / `-Compiler clang++`。

## 用法

```console
$ aec hello.ae              # 编译 → hello.aeo
$ ae hello.aeo 参数1 参数2   # 运行，后面的参数给脚本
$ aed hello.aeo             # 看字节码
```

AOT（把 `.ae` 转成 C++ 源码）：

```console
$ aec hello.ae --aot -o hello.cpp     # 只生成 C++，自己拿去编
$ aec hello.ae --aot-exe -O2          # 直接编出可执行文件
```

`--aot-exe` 分三种情况：

| 环境 | 行为 |
|---|---|
| 构建时链入了 Clang + LLD | 进程内编译，不需要任何外部编译器 |
| 没链入，但系统有 g++ / clang++ | 自动调用外部编译器 |
| 都没有 | 保留 `.cpp`，自行编译；也可用 `--cc=<命令>` 指定 |

想让 `aec` 自带编译器（进程内编译，不依赖外部程序），先装 LLVM/Clang 套件再构建，脚本会自动探测 `llvm-config`：

```bash
pkg install clang llvm lld libllvm-static    # Termux / Debian
brew install llvm                            # macOS
LLVM_CONFIG=/path/to/llvm-config ./build.sh  # 不在默认位置时
```

Windows 上 MSVC 不启用此功能，请用 `clang++` 或 MinGW 的 `g++`。

AOT 已知限制：不支持协程（`coroutine.*`）与 `run()`——两者依赖切换调用栈，C++ 栈无法 unwind，遇到会编译期报错。其余特性与 VM 后端输出一致。

## 常见问题

**模块找不到（E1014）**

`.m` 模块按这个顺序找（`<exe>` 是可执行文件所在目录，`<script>` 是脚本所在目录）：

```
<exe>/lib → <exe> → <script>/lib → <script> → ./lib → ./ → $AE_LIB_PATH
```

内置库 `io` `os` `path` 编在可执行文件里，`imp io` 直接用。

**中文乱码**

Ae 全程 UTF-8。Windows 控制台由 `ae_console.h` 统一处理。终端字体不支持中文时照样是方框，那是终端的问题。

**`/` 对整数是整除吗**

是。`7 / 2` 得 `3`，要浮点写 `7 / 2.0` 或 `float(7) / 2`。

**`==` 为什么两张内容相同的表不相等**

`==` 对表、函数比身份，对标量比值。比内容用 `equal(a, b)`。

**`a[1:3]` 到底包含几个元素**

切片是闭区间、1-based，`a[1:3]` 是第 1 到第 3 个，共三个。`a[::-1]` 反转。

**Windows 上跑不了自己编的可执行文件**

别放共享存储。`C:\Users\...` 这类用户目录可写可执行；Termux 同理用 `~/`，不要用 `/storage/emulated/0/`（那个分区是 noexec）。

**测试怎么跑**

```bash
cd Test && ./run.sh              # 97 个用例
./run.sh -Filter p13             # 只跑名字含 p13 的
```

Windows 用 `run.ps1`。加用例就是往 `cases/` 或 `errors/` 放一对文件，不用注册。

---

## 关于这个项目

Ae 是个人项目，从词法分析到虚拟机、从标准库到 AOT 后端，全部手写，没有依赖任何编译器框架的实现代码——Clang/LLD 只是作为可选的编译后端被链接进来。

整个项目是在手机上用 C4Droid 写完的。这也解释了为什么 AOT 一定要把编译器链进 `aec` 本体：安卓上应用私有目录能跑单个二进制，但再 fork 出第二个程序会撞权限，进程外调用编译器那条路走不通。

设计上一直守着两条：

- **不堆特性**。20 个关键字够用就不加，不做面向对象体系。
- **报错要说人话**。每个错误都带错误码、源码摘录、光标位置和 help，宁可编译器写复杂点，也不让人猜。

## 计划

持续更新中，下面是还在推进或打算做的：

- [ ] REPL（逐步推进中）
- [ ] 拓展标准库：更多实用模块
- [ ] 拓展语法特性：语言层面新增能力
- [ ] 拓展语法糖：让常见写法更短
- [ ] 包管理（长远，需要生态）

已完成：

- [x] AOT 编译器（Ae → C++，已完成）
- [x] 字节码虚拟机后端

遗弃：

- ~~OOP（遗弃）~~
- ~~VM 引入 JIT（遗弃，已有 AOT 路线）~~

## 引用与声明

- 字节码格式 `AeBc 1.8` 与 Ae 语言本身均为本项目原创设计
- Clang / LLVM / LLD 为 LLVM 基金会项目，以库的形式可选链入，用于 AOT 的进程内编译；LLVM 许可证见 <https://llvm.org/LICENSE.txt>
- `math.m` 中的数值算法参考 IEEE-754 与标准 C `libm` 的行为，精度 ≤ 4 ulp

Ae 本身采用 MIT 许可证，全文见 [LICENSE](LICENSE)。

当前版本 **2.1.0**。
