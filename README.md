# Ae
**Ae**（读作 /iː/，形如古英语 æsc）是一门轻量级编程语言，目标用于嵌入式设备。  
它采用经典的三段式架构：**编译器 → 字节码 → 虚拟机**，并附带一个反汇编工具，方便观察中间表示。

```
.ae 源文件  ──编译──▶  .aeo 字节码  ──执行──▶  运行结果
                    └──反汇编──▶  可读汇编
```

---

## 特性一览

- **静态三件套**：编译器 `aec`、虚拟机 `ae`、反汇编器 `aed`，全部用 C++ 写成，零第三方依赖
- **自定义字节码格式** `.aeo`，文件头含魔数 `AeBc`、版本号、常量池与代码段
- **Tagged Value 运行时**：`int` / `double` / `string` / `bool` 四种类型在栈上带标签运行
- **支持控制流**：`if / else`、`while`、`break`、`continue`
- **复合赋值**：`+= -= *= /=`
- **自动类型提升**：`int` 与 `double` 混合运算时自动提升为浮点
- **内置输出**：`pr(...)` 可打印任意类型，支持多参数

---

## 组件说明

| 工具 | 源文件 | 作用 |
|------|--------|------|
| **aec** (Ae Compiler) | `aec.cpp` | 词法分析 + 递归下降编译，把 `.ae` 编译为 `.aeo` |
| **ae**  (Ae VM)       | `ae.cpp`  | 读取 `.aeo`，解析字节码并执行 |
| **aed** (Ae Disassembler) | `aed.cpp` | 把 `.aeo` 反汇编为可读的汇编清单 |

> 三个程序均可独立编译，不依赖彼此。

---

## 构建

需要支持 C++11 及以上的编译器（g++ / clang++）。

```bash
# 编译三个工具
g++ -std=c++11 -O2 aec.cpp -o aec
g++ -std=c++11 -O2 ae.cpp  -o ae
g++ -std=c++11 -O2 aed.cpp -o aed
```

也可写一个简单的 `Makefile`：

```makefile
CXX = g++
CXXFLAGS = -std=c++11 -O2

all: aec ae aed

aec: aec.cpp
	$(CXX) $(CXXFLAGS) $< -o $@
ae: ae.cpp
	$(CXX) $(CXXFLAGS) $< -o $@
aed: aed.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f aec ae aed
```

---

## 快速开始

### 1. 编写一个 Ae 程序 `hello.ae`

```ae
main {
    pr("Hello, Ae!")
}
```

### 2. 编译为字节码

```bash
./aec hello.ae
# ✓ 编译成功: hello.ae → hello.aeo
```

### 3. 运行

```bash
./ae hello.aeo
# Hello, Ae!
```

### 4. （可选）反汇编查看字节码

```bash
./aed hello.aeo
```

输出：

```
;  Ae Bytecode Disassembly  (hello.aeo)
;  Magic: AeBc     Version: 1.0      Flags: 0x0

; ── Constant Pool (1 entries) ───────────────────────
  #  0  utf8   "Hello, Ae!"

; ── Code (...) ──────────────────────────────────
     0: LOAD_CONST  #0
     2: CALL       pr  argc=1
     5: HALT
```

---

## 语言语法

### 程序结构

程序由可选的全局语句和一个 `main { ... }` 块组成：

```ae
main {
    // 你的代码
}
```

### 数据类型

| 类型 | 字面量示例 | 说明 |
|------|-----------|------|
| `int` | `42`, `-7` | 64 位整数 |
| `double` | `3.14`, `.5` | 双精度浮点 |
| `string` | `"hello"`, `'world'` | 双引号或单引号，支持转义 |
| `bool` | 比较表达式结果 | `true` / `false` |

### 变量与赋值

```ae
main {
    x = 10
    y = 3.14
    name = "Ae"

    x += 1      // x 变为 11
    y *= 2.0    // y 变为 6.28
    x -= 3      // x 变为 8
}
```

### 运算符

**优先级从高到低**：

```
一元负号(-)  >  乘除模(* / %)  >  加减(+ -)
>  比较(> < >= <= == !=)  >  复合赋值(+= -= *= /=)  >  赋值(=)
```

比较运算支持 `int-int`、`double-double` 以及混合 `int-double`（自动提升为浮点比较）。

```ae
main {
    a = 10
    b = 3
    pr(a + b * 2)        // 16
    pr(a / b)            // 3 (整数除法)
    pr(a > b)            // true
    pr(2.5 * 4)          // 10.0
}
```

### 控制流

**if / else：**

```ae
main {
    x = 7
    if (x > 5) {
        pr("big")
    } else {
        pr("small")
    }
}
```

**while + break / continue：**

```ae
main {
    i = 0
    while (i < 5) {
        i += 1
        if (i == 3) continue
        pr(i)
    }
}
```

> 支持嵌套循环，`break` / `continue` 始终作用于最内层循环。

### 输出函数 `pr`

`pr` 是内置函数（funcId = 0），可接收任意数量的参数，参数间以空格分隔：

```ae
main {
    name = "Ae"
    version = 1.0
    pr("lang:", name, "v", version)
    // lang: Ae v1
}
```

---

## 字节码规范（.aeo）

### 文件头（16 字节）

| offset | size | field |
|--------|------|-------|
| 0      | 4    | magic = `"AeBc"` |
| 4      | 1    | major = 1 |
| 5      | 1    | minor = 0 |
| 6      | 2    | flags = 0 |
| 8      | 4    | constPoolCount (u32, 大端) |
| 12     | 4    | codeLength (u32, 大端) |

### 常量池

`constPoolCount` 个条目紧凑排列：

```
tag (1 byte) + length (4 byte, 大端) + data
```

- `0x01` UTF-8 字符串
- `0x02` int32
- `0x03` double (64-bit，大端)

### 指令集

| OpCode | 助记符 | 操作数 | 语义 |
|--------|--------|--------|------|
| `0x00` | NOP | — | 无操作 |
| `0x01` | LOAD_CONST | u16(idx) | 栈压入常量池[idx] |
| `0x02` | CALL | u8(funcId) u8(argc) | 调用函数 |
| `0x03` | HALT | — | 停机 |
| `0x04` | STORE | u16(slot) | 栈顶 → 全局变量[slot] |
| `0x05` | LOAD_VAR | u16(slot) | 全局变量[slot] → 栈顶 |
| `0x06`–`0x09` | IADD / ISUB / IMUL / IDIV | — | 整数四则 |
| `0x0A`–`0x0D` | FADD / FSUB / FMUL / FDIV | — | 浮点四则 |
| `0x0E` | ITOD | — | int64 → double |
| `0x0F` | IMOD | — | 整数取模 |
| `0x10`–`0x15` | IEQ / INEQ / ILT / ILE / IGT / IGE | — | 整数比较 |
| `0x16`–`0x1B` | FCMP_EQ / NE / LT / LE / GT / GE | — | 浮点比较 |
| `0x2A` | JZ | u16(offset) | 栈顶为 false/0 时跳转 (int16 有符号偏移) |
| `0x2B` | JMP | u16(offset) | 无条件跳转 (int16 有符号偏移) |

> 注：`0x2C` (BREAK) / `0x2D` (CONTINUE) 仅存在于编译期，链接阶段会被重写为 `JMP + u16 偏移`，运行时只识别 `JZ` / `JMP`。

---

## 仓库结构

```
ae/
├── README.md          # 本文件
├── LICENSE            # MIT License
├── aec.cpp            # 编译器
├── ae.cpp             # 虚拟机
├── aed.cpp            # 反汇编器
└── examples/          # 示例程序
    ├── hello.ae
    └── test.ae
```

---

## Roadmap

- [ ] 函数定义（目前仅有内置 `pr`）
- [ ] 局部变量与作用域
- [ ] 数组 / 列表类型
- [ ] 字符串内置方法
- [ ] 更丰富的标准库

---

## License

本项目基于 [MIT License](./LICENSE) 开源。

---

> Made with ♡ by the Ae community.
