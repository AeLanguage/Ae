// =============================================================================
//  Ae 诊断系统  ——  ae_diag.h
// -----------------------------------------------------------------------------
//  目标：像正常编程语言那样报错 —— 有【错误码】、【错误类别】、源码摘录、
//        波浪线定位、以及可选 help/note。全部只用 ASCII 标记（[-] [!] [+]），
//        不依赖 ✗ 这类符号，任何终端/编码环境都能正常显示。
//
//  输出格式：
//      [-] error[E1006]: 未声明的变量 'pritn'
//          --> test.ae:4:8
//           |
//         4 |     prln(pritn)
//           |          ^^^^^
//           |
//           = help: 赋值可隐式创建全局；函数内请先用 local 声明
//
//       运行期错误额外带调用栈：
//           = stack (最近调用在前)
//             level3  fib.ae:2:18
//             level2  fib.ae:5:12
//
//  码段约定：
//      E1xxx  编译期（词法/语法/名称/类型/导入）
//      E2xxx  运行期（类型/除零/索引/字节码/原生库）
//      E0xxx  工具链自身（用法、IO、内部错误）
// =============================================================================
#ifndef AE_DIAG_H
#define AE_DIAG_H

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
#endif
#include <string>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <algorithm>

// ── 错误码：编译期 ──
#define AE_E_SYNTAX        "E1001"   // 通用语法错误
#define AE_E_BADCHAR       "E1002"   // 非法字符
#define AE_E_STR_UNCLOSED  "E1003"   // 字符串未闭合
#define AE_E_CMT_UNCLOSED  "E1004"   // 块注释未闭合
#define AE_E_UNDEF_FUNC    "E1005"   // 未定义的函数
#define AE_E_UNDECLARED    "E1006"   // 未声明的变量
#define AE_E_UNDEF_VAR     "E1007"   // 复合赋值目标未定义
#define AE_E_DUP_FUNC      "E1008"   // 函数重复定义
#define AE_E_SHADOW_BUILTIN "E1009"  // 遮蔽内建名
#define AE_E_ARITY         "E1010"   // 函数参数个数不匹配
#define AE_E_LIB_ARITY     "E1011"   // 库函数参数个数不匹配
#define AE_E_LIB_NOFUNC    "E1012"   // 库里没有该函数
#define AE_E_NO_IMPORT     "E1013"   // 未导入的库
#define AE_E_LIB_NOTFOUND  "E1014"   // 找不到库
#define AE_E_LIB_ABI       "E1015"   // 库 ABI 版本不匹配
#define AE_E_NUM_RANGE     "E1016"   // 数值常量越界
#define AE_E_BAD_UTF8      "E1017"   // 源码不是合法 UTF-8
#define AE_E_LOOP_CTRL     "E1018"   // break/continue 用法错误
#define AE_E_SCOPE         "E1019"   // local/return 位置错误
#define AE_E_MULTI_ASSIGN  "E1020"   // 赋值目标个数不匹配
#define AE_E_TABLE_LIT     "E1021"   // 表字面量语法
#define AE_E_MAIN_SIG      "E1022"   // main 签名非法
#define AE_E_SECTION       "E1023"   // 字节码/编译内部错误
// ── 错误码：运行期 ──
#define AE_E_TYPE          "E2001"   // 类型不匹配
#define AE_E_DIV_ZERO      "E2002"   // 除零
#define AE_E_INDEX         "E2003"   // 表索引/键非法
#define AE_E_BC_FUNC       "E2004"   // 调用了未定义的函数（字节码）
#define AE_E_BC_CORRUPT    "E2005"   // 字节码损坏
#define AE_E_STACK_OVER    "E2006"   // 调用栈溢出
#define AE_E_ASSERT        "E2007"   // assert 失败
#define AE_E_USER_ERROR    "E2008"   // 脚本主动 error()
#define AE_E_NATIVE        "E2009"   // 原生库报错
#define AE_E_INPUT         "E2010"   // 输入读取失败
#define AE_E_INTERNAL      "E2011"   // VM 内部错误
// ── 错误码：工具链 ──
#define AE_E_USAGE         "E0001"   // 命令行用法错误
#define AE_E_FILE          "E0002"   // 文件读写/加载失败

namespace aediag {

struct AeSpan {
    std::string file;
    int line = 1;
    int col  = 1;      // 1-based，按【字节】计数（与词法器一致），打印时转显示列
    int len  = 1;      // 下划线长度（字节）
};

struct AeDiag {
    std::string code = AE_E_INTERNAL;
    std::string kind = "error";          // error / warning
    std::string msg;                     // 主消息
    std::string label;                   // 波浪线后面的短标签（可空）
    std::vector<std::string> helps;      // = help: 行
    std::vector<std::string> stackLines; // 运行期调用栈（已格式化）
    AeSpan span;
    bool hasSpan = false;
};

class AeError : public std::runtime_error {
public:
    AeDiag diag;
    explicit AeError(const AeDiag& d) : std::runtime_error(d.msg), diag(d) {}
};

// ABI 稳定的小工具：给库作者以外的地方用
inline AeDiag make(const std::string& code, const std::string& msg) {
    AeDiag d; d.code = code; d.msg = msg; return d;
}
[[noreturn]] inline void raise(const std::string& code, const std::string& msg) {
    throw AeError(make(code, msg));
}

// ── 源码摘录工具 ──
inline std::string lineAt(const std::string& src, int line) {
    if (line < 1 || src.empty()) return std::string();
    int cur = 1;
    size_t start = 0;
    for (size_t i = 0; i <= src.size(); i++) {
        if (i == src.size() || src[i] == '\n') {
            if (cur == line) {
                size_t end = i;
                if (end > start && src[end - 1] == '\r') end--;
                return src.substr(start, end - start);
            }
            cur++;
            start = i + 1;
            if (i == src.size()) break;
        }
    }
    return std::string();
}

// 一行里第 byteCol 个字节对应的【显示列】（制表符按 4 空格展开，UTF-8 按码点算 1 列）
// ★ 修（CJK）：中文/全角字符在终端和编辑器里占 2 列，以前一律算 1 列 —— 于是
//   「一行里前面有中文」时，^ 光标会比目标字符偏左（差几个字就偏几个字符位）。
//   现在按显示宽度计算，和终端里看到的对齐。
inline int cpWidth(unsigned cp) {
    if (cp == 0) return 0;
    // 组合符号 / 零宽：不占列
    if ((cp >= 0x0300 && cp <= 0x036F) || cp == 0x200B || cp == 0x200C || cp == 0x200D) return 0;
    // 东亚全角（范围取自 Unicode EastAsianWidth = W/F 的常用区段）
    if ((cp >= 0x1100 && cp <= 0x115F) || cp == 0x2329 || cp == 0x232A ||
        (cp >= 0x2E80 && cp <= 0x303E) || (cp >= 0x3041 && cp <= 0x33FF) ||
        (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
        (cp >= 0xA000 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE10 && cp <= 0xFE19) ||
        (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x1F300 && cp <= 0x1F64F) || (cp >= 0x1F900 && cp <= 0x1F9FF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD))
        return 2;
    return 1;
}
// 从字节流里读一个 UTF-8 码点，返回其字节数与码点
inline size_t utf8Decode(const std::string& s, size_t i, unsigned* cpOut) {
    unsigned char b0 = (unsigned char)s[i];
    size_t n = (b0 < 0x80) ? 1 : ((b0 & 0xE0) == 0xC0) ? 2 : ((b0 & 0xF0) == 0xE0) ? 3
             : ((b0 & 0xF8) == 0xF0) ? 4 : 1;
    if (n > s.size() - i) n = 1;
    unsigned cp = b0;
    if (n > 1) {
        cp = b0 & (0xFF >> (n + 1));
        for (size_t k = 1; k < n; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
    }
    if (cpOut) *cpOut = cp;
    return n;
}
inline int displayCol(const std::string& line, int byteCol, std::string* expandedOut) {
    std::string out;
    int disp = 0;
    int byteIdx = 1;
    size_t i = 0;
    while (i < line.size()) {
        if (byteIdx >= byteCol) break;
        unsigned char c = (unsigned char)line[i];
        if (c == '\t') {
            int pad = 4 - (disp % 4);
            out.append((size_t)pad, ' ');
            disp += pad;
            byteIdx += 1;
            i += 1;
            continue;
        }
        unsigned cp = 0;
        size_t n = utf8Decode(line, i, &cp);
        out.append(line, i, n);
        disp += cpWidth(cp);
        byteIdx += (int)n;
        i += n;
    }
    // 余下部分也要展开制表符（保持整行显示一致）
    while (i < line.size()) {
        unsigned char c = (unsigned char)line[i];
        if (c == '\t') {
            int pad = 4 - (disp % 4);
            out.append((size_t)pad, ' ');
            disp += pad;
            i += 1;
            continue;
        }
        size_t n = utf8Decode(line, i, nullptr);
        out.append(line, i, n);
        i += n;
    }
    if (expandedOut) *expandedOut = out;
    return disp;
}

// 一段字节在终端里占多少个显示列（决定 ^^^ 要画多长）
inline int spanWidth(const std::string& line, int byteCol, int byteLen) {
    int byteIdx = 1;
    size_t i = 0;
    while (i < line.size() && byteIdx < byteCol) {
        size_t n = utf8Decode(line, i, nullptr);
        i += n; byteIdx += (int)n;
    }
    int endByte = byteCol + (std::max)(0, byteLen);
    int cols = 0;
    while (i < line.size() && byteIdx < endByte) {
        unsigned cp = 0;
        size_t n = utf8Decode(line, i, &cp);
        cols += cpWidth(cp);
        i += n; byteIdx += (int)n;
    }
    return (std::max)(1, cols);
}

// 旧的“按码点算”的版本：仅保留给外部调用，内部已改用 spanWidth
inline int spanChars(const std::string& line, int byteCol, int byteLen) {
    return spanWidth(line, byteCol, byteLen);
}

// ── 打印一条诊断 ──
inline void printDiag(const AeDiag& d, const std::string& src) {
    std::cerr << "[-] " << d.kind << "[" << d.code << "]: " << d.msg << "\n";
    if (d.hasSpan) {
        std::string raw = lineAt(src, d.span.line);
        // ★ 列号按【显示列】打印：中文算 2 列，和你编辑器状态栏里的列号一致；
        //   内部 span.col 仍是字节列，只在对外显示时换算
        int showCol = d.span.col;
        if (!raw.empty()) showCol = displayCol(raw, d.span.col, nullptr) + 1;
        std::cerr << "    --> " << d.span.file << ":" << d.span.line << ":" << showCol << "\n";
        if (!raw.empty()) {
            std::string expanded;
            int dispCol = displayCol(raw, d.span.col, &expanded);
            int caretLen = spanWidth(raw, d.span.col, d.span.len);
            std::string num = std::to_string(d.span.line);
            std::string pad(num.size(), ' ');
            std::cerr << "     " << pad << " |\n";
            std::cerr << "     " << num << " | " << expanded << "\n";
            std::string caret((size_t)dispCol, ' ');
            caret.append((size_t)caretLen, '^');
            if (!d.label.empty()) caret += " " + d.label;
            std::cerr << "     " << pad << " | " << caret << "\n";
        }
    }
    for (const std::string& h : d.helps)
        std::cerr << "     = help: " << h << "\n";
    if (!d.stackLines.empty()) {
        std::cerr << "     = stack (最近调用在前)\n";
        for (const std::string& s : d.stackLines)
            std::cerr << "       " << s << "\n";
    }
    std::cerr << std::flush;
}

// 不含源码摘录的一行式（给 VM 启动/用法类错误）
inline void printBrief(const std::string& code, const std::string& msg,
                       const std::string& kind = "error") {
    std::cerr << "[-] " << kind << "[" << code << "]: " << msg << "\n" << std::flush;
}
inline void printOk(const std::string& msg) {
    std::cout << "[+] " << msg << "\n" << std::flush;
}

}  // namespace aediag

#endif  // AE_DIAG_H
