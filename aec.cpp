// =============================================================================
//  Ae 编译器 (aec = Ae Compiler)
// -----------------------------------------------------------------------------
//  读取 .ae 源文件 → 编译为 .aeo 字节码
//
//  ▌字节码规范 (Ae Bytecode, .aeo)
//  ──────────────────────────────────────────────────────────────────────────
//  【文件头】 16 字节
//      offset  size   field
//      0       4      magic      = "AeBc"
//      4       1      major     = 1
//      5       1      minor     = 0
//      6       2      flags     = 0
//      8       4      constPoolCount  (u32, 大端)
//      12      4      codeLength      (u32, 大端)
//
//  【常量池】 constPoolCount 条目，紧凑排列
//      tag (1 byte) + length (4 byte, 大端) + data
//      tag: 0x01 UTF-8  |  0x02 int32  |  0x03 double
//
//  【字节码流】 codeLength 字节
//      OpCode  助记符       操作数             语义
//      0x00    NOP           -                无操作
//      0x01    LOAD_CONST   u16(idx)          栈压入常量池[idx]
//      0x02    CALL         u8(funcId) u8(argc) 调用函数, 参数从栈弹出
//      0x03    HALT          -                停机
//      0x04    STORE        u16(slot)         栈顶 → 全局变量[slot]
//      0x05    LOAD_VAR     u16(slot)         全局变量[slot] → 栈顶
//      0x06    IADD          -                栈弹出 b,a → (a+b) int
//      0x07    ISUB          -                栈弹出 b,a → (a-b) int
//      0x08    IMUL          -                栈弹出 b,a → (a*b) int
//      0x09    IDIV          -                栈弹出 b,a → (a/b) int
//      0x0A    FADD          -                浮点加
//      0x0B    FSUB          -                浮点减
//      0x0C    FMUL          -                浮点乘
//      0x0D    FDIV          -                浮点除
//      0x0E    ITOD          -                栈顶 int64 → double
//      0x0F    IMOD          -                栈弹出 b,a → (a%b) int
//      0x10    IEQ           -                栈弹出 b,a → (a==b) bool
//      0x11    INEQ          -                栈弹出 b,a → (a!=b) bool
//      0x12    ILT           -                栈弹出 b,a → (a<b) bool
//      0x13    ILE           -                栈弹出 b,a → (a<=b) bool
//      0x14    IGT           -                栈弹出 b,a → (a>b) bool
//      0x15    IGE           -                栈弹出 b,a → (a>=b) bool
//      0x16    FCMP_EQ       -                浮点 == (a,b 为 double)
//      0x17    FCMP_NE       -                浮点 !=
//      0x18    FCMP_LT       -                浮点 <
//      0x19    FCMP_LE       -                浮点 <=
//      0x1A    FCMP_GT       -                浮点 >
//      0x1B    FCMP_GE       -                浮点 >=
//      0x2A    JZ           u16(offset)      栈顶 false/0 时跳转（offset 为有符号 int16）
//      0x2B    JMP          u16(offset)      无条件跳转（offset 为有符号 int16）
//      注: 0x2C(BREAK)/0x2D(CONTINUE) 仅为编译期占位指令，
//          链接阶段被统一改写为 0x2B(JMP) + u16 偏移，运行时只识别 JZ/JMP。
//
//  ▌类型系统
//  --------------------------------------------------------------------------
//  运行时类型: int, double, string, bool
//  比较运算: 支持 int-int, double-double, 以及混合 int-double（自动提升）
//  比较结果: bool 值 (VAL_BOOL)，可存储到变量、传递给 pr()/prln()
//
//  ▌表达式优先级 (从高到低)
//      primary → 一元 - → 乘除模 → 加减 → 比较(> < >= <= == !=)
//      → 复合赋值(+= -= *= /=)  →  赋值(=)
// =============================================================================

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

using namespace std;

// =============================================================================
//  字节码写入辅助（大端）
// =============================================================================
static void writeU16(vector<uint8_t>& v, uint16_t val) {
    v.push_back((val >> 8) & 0xFF); v.push_back(val & 0xFF);
}
static void writeU32(vector<uint8_t>& v, uint32_t val) {
    v.push_back((val >> 24) & 0xFF); v.push_back((val >> 16) & 0xFF);
    v.push_back((val >> 8) & 0xFF); v.push_back(val & 0xFF);
}

// =============================================================================
//  常量池
// =============================================================================
struct Const {
    enum { UTF8 = 1, INT32 = 2, DOUBLE = 3 } tag;
    string  s;   // UTF8
    int32_t i;   // INT32
    double  d;   // DOUBLE
};

class ConstantPool {
public:
    vector<Const> pool;

    uint16_t addString(const string& s) {
        for (uint16_t k = 0; k < (uint16_t)pool.size(); k++)
            if (pool[k].tag == Const::UTF8 && pool[k].s == s) return k;
        Const c; c.tag = Const::UTF8; c.s = s; pool.push_back(c); return (uint16_t)pool.size() - 1;
    }
    uint16_t addInt(int32_t v) {
        for (uint16_t k = 0; k < (uint16_t)pool.size(); k++)
            if (pool[k].tag == Const::INT32 && pool[k].i == v) return k;
        Const c; c.tag = Const::INT32; c.i = v; pool.push_back(c); return (uint16_t)pool.size() - 1;
    }
    uint16_t addDouble(double v) {
        for (uint16_t k = 0; k < (uint16_t)pool.size(); k++)
            if (pool[k].tag == Const::DOUBLE && pool[k].d == v) return k;
        Const c; c.tag = Const::DOUBLE; c.d = v; pool.push_back(c); return (uint16_t)pool.size() - 1;
    }

    void write(vector<uint8_t>& out) const {
        writeU32(out, (uint32_t)pool.size());
        for (const Const& c : pool) {
            out.push_back((uint8_t)c.tag);
            if (c.tag == Const::UTF8) {
                writeU32(out, (uint32_t)c.s.size());
                out.insert(out.end(), c.s.begin(), c.s.end());
            } else if (c.tag == Const::INT32) {
                writeU32(out, (uint32_t)(uint32_t)c.i);
            } else if (c.tag == Const::DOUBLE) {
                uint64_t bits; memcpy(&bits, &c.d, 8);
                for (int s = 56; s >= 0; s -= 8) out.push_back((uint8_t)((bits >> s) & 0xFF));
            }
        }
    }
};

// =============================================================================
//  字节码缓冲区
// =============================================================================
struct CodeBuf {
    vector<uint8_t> code;
    void emit(uint8_t b) { code.push_back(b); }
    void u8(uint8_t v)  { code.push_back(v); }
    void u16(uint16_t v){ writeU16(code, v); }
    void write(vector<uint8_t>& out) const {
        writeU32(out, (uint32_t)code.size());
        out.insert(out.end(), code.begin(), code.end());
    }
};

// =============================================================================
//  符号表
// =============================================================================
class SymbolTable {
public:
    map<string, uint16_t> vars;   // name → slot
    uint16_t alloc(const string& name) {
        auto it = vars.find(name);
        if (it != vars.end()) return it->second;
        uint16_t slot = (uint16_t)vars.size(); vars[name] = slot; return slot;
    }
    uint16_t get(const string& name) const {
        auto it = vars.find(name);
        if (it != vars.end()) return it->second;
        return (uint16_t)-1;
    }
};

// =============================================================================
//  词法分析
// =============================================================================
enum TK {
    TK_EOF, TK_NUM, TK_STR, TK_IDENT, TK_ASSIGN,
    TK_PLUS, TK_MINUS, TK_MUL, TK_DIV, TK_MOD,
    TK_EQ, TK_NEQ, TK_LT, TK_LE, TK_GT, TK_GE,   // 比较运算符
    TK_ADDEQ, TK_SUBEQ, TK_MULEQ, TK_DIVEQ,       // 复合赋值 += -= *= /=
    TK_IF, TK_ELSE, TK_WHILE, TK_BREAK, TK_CONTINUE, // 控制流关键字
    TK_LPAREN, TK_RPAREN, TK_COMMA, TK_LBRACE, TK_RBRACE, TK_OTHER
};

struct Token {
    TK type; string text; double num; bool isFloat;
    Token() : type(TK_EOF), num(0), isFloat(false) {}
    Token(TK t, const string& tx = "", double n = 0, bool f = false)
        : type(t), text(tx), num(n), isFloat(f) {}
};

class Lexer {
    string src; size_t pos;
public:
    Lexer(const string& s) : src(s), pos(0) {}
    void skip() {
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n' || src[pos] == '\r')) pos++;
    }
    Token next() {
        skip();
        if (pos >= src.size()) return {TK_EOF};
        char c = src[pos];

        // 注释
        if (c == '/' && pos + 1 < src.size() && src[pos+1] == '/') {
            while (pos < src.size() && src[pos] != '\n') pos++;
            return next();
        }
        // 字符串
        if (c == '"' || c == '\'') {
            char q = c; pos++;
            string s;
            while (pos < src.size() && src[pos] != q) {
                if (src[pos] == '\\' && pos + 1 < src.size()) { s += src[++pos]; }
                else s += src[pos];
                pos++;
            }
            if (pos < src.size()) pos++;
            return {TK_STR, s, 0, false};
        }
        // 数字
        if (isdigit(c) || (c == '.' && pos+1 < src.size() && isdigit(src[pos+1]))) {
            string num; bool dot = false;
            if (c == '.') { num += '0'; }
            while (pos < src.size() && (isdigit(src[pos]) || src[pos] == '.')) {
                if (src[pos] == '.') { if (dot) break; dot = true; }
                num += src[pos++];
            }
            Token t; t.type = TK_NUM; t.text = num; t.isFloat = dot;
            t.num = dot ? stod(num) : (double)stoi(num); return t;
        }
        // 标识符 / 关键字
        if (isalpha(c) || c == '_') {
            string id; while (pos < src.size() && (isalnum(src[pos]) || src[pos] == '_')) id += src[pos++];
            // 关键字
            if (id == "if")       return {TK_IF, id};
            if (id == "else")     return {TK_ELSE, id};
            if (id == "while")    return {TK_WHILE, id};
            if (id == "break")    return {TK_BREAK, id};
            if (id == "continue") return {TK_CONTINUE, id};
            if (id == "pr")          return {TK_IDENT, id}; // pr 是内置函数，词法上当标识符
            if (id == "inp")         return {TK_IDENT, id}; // inp 是内置函数
            if (id == "prln")       return {TK_IDENT, id}; // prln 是内置函数
            if (id == "main")        return {TK_IDENT, id};
            return {TK_IDENT, id, 0, false};
        }
        pos++;
        // 多字符运算符（优先匹配）
        if (c == '=' && pos < src.size() && src[pos] == '=') { pos++; return {TK_EQ, "==", 0, false}; }
        if (c == '!' && pos < src.size() && src[pos] == '=') { pos++; return {TK_NEQ, "!=", 0, false}; }
        if (c == '<' && pos < src.size() && src[pos] == '=') { pos++; return {TK_LE, "<=", 0, false}; }
        if (c == '>' && pos < src.size() && src[pos] == '=') { pos++; return {TK_GE, ">=", 0, false}; }
        // 复合赋值（优先匹配双字符）
        if (c == '+' && pos < src.size() && src[pos] == '=') { pos++; return {TK_ADDEQ, "+=", 0, false}; }
        if (c == '-' && pos < src.size() && src[pos] == '=') { pos++; return {TK_SUBEQ, "-=", 0, false}; }
        if (c == '*' && pos < src.size() && src[pos] == '=') { pos++; return {TK_MULEQ, "*=", 0, false}; }
        if (c == '/' && pos < src.size() && src[pos] == '=') { pos++; return {TK_DIVEQ, "/=", 0, false}; }
        switch (c) {
            case '=': return {TK_ASSIGN, "=", 0, false};
            case '+': return {TK_PLUS, "+", 0, false};
            case '-': return {TK_MINUS, "-", 0, false};
            case '*': return {TK_MUL, "*", 0, false};
            case '/': return {TK_DIV, "/", 0, false};
            case '%': return {TK_MOD, "%", 0, false};
            case '<': return {TK_LT, "<", 0, false};
            case '>': return {TK_GT, ">", 0, false};
            case '(': return {TK_LPAREN, "(", 0, false};
            case ')': return {TK_RPAREN, ")", 0, false};
            case ',': return {TK_COMMA, ",", 0, false};
            case '{': return {TK_LBRACE, "{", 0, false};
            case '}': return {TK_RBRACE, "}", 0, false};
            default:  return {TK_OTHER, string(1, c), 0, false};
        }
    }
};

// =============================================================================
//  编译器（递归下降 + 代码生成）
// =============================================================================
class Compiler {
public:
    Lexer    L; Token cur;
    ConstantPool cp;
    SymbolTable  syms;
    CodeBuf  code;

    // ---- break/continue 回填上下文（支持嵌套循环） ----
    struct LoopCtx {
        size_t startPos;               // 条件计算起始 pc（continue 跳回这里）
        vector<size_t> breakPatches;   // BREAK 占位位置，待回填到循环结束
        vector<size_t> contPatches;    // CONTINUE 占位位置，待回填到条件处
    };
    vector<LoopCtx> loopStack;

    Compiler(const string& src) : L(src) { advance(); }
    void advance() { cur = L.next(); }

    void expect(TK t, const string& m) {
        if (cur.type != t) throw runtime_error("语法错误: 期望 " + m + "，得到 '" + cur.text + "'");
        advance();
    }

    // ---- 占位 / 回填工具 ----
    // 占位：先 emit 0x00，记录位置，稍后用 patchU8/patchU16 回填
    size_t placeholderU8() { code.emit(0x00); return code.code.size() - 1; }
    size_t placeholderU16() { code.u16(0x0000); return code.code.size() - 2; }

    void patchU8(size_t pos, uint8_t val) { code.code[pos] = val; }
    void patchU16(size_t pos, uint16_t val) { writeU16At(code.code, pos, val); }

    static void writeU16At(vector<uint8_t>& v, size_t pos, uint16_t val) {
        v[pos]   = (val >> 8) & 0xFF;
        v[pos+1] = val & 0xFF;
    }

    // 计算从「占位点」到「目标 pc」的 JZ/JMP 偏移量（u16，含操作数自身 2 字节）
    // 支持正向（if/while 跳过）与反向（while 回环）跳转：
    //   offset = target - (placeholderPos + 2)，以 int16 解释，存为 uint16 补码
    uint16_t jmpOffset(size_t placeholderPos, size_t targetPos) {
        int64_t off = (int64_t)targetPos - (int64_t)(placeholderPos + 2);
        if (off < -32768 || off > 32767) throw runtime_error("跳转跨度过大（超出 int16）");
        return (uint16_t)(int16_t)off;  // 负偏移 → 补码，运行时 pc += off 自动回退
    }

    // =========================================================================
    //  表达式编译
    // =========================================================================
    bool expr(bool ctx) { return compare(ctx); }

    // compare 层：处理 == != < <= > >=
    bool compare(bool needDouble) {
        (void)needDouble; // 比较层目前统一按类型提升处理，参数预留
        bool ld = addsub(false);

        if (cur.type >= TK_EQ && cur.type <= TK_GE) {
            TK op = cur.type; advance();
            bool rd = addsub(false);
            bool isDouble = ld || rd;

            if (isDouble) {
                if (!ld) code.emit(0x0E);
                if (!rd) code.emit(0x0E);
                switch (op) {
                    case TK_EQ:  code.emit(0x16); break;
                    case TK_NEQ: code.emit(0x17); break;
                    case TK_LT:  code.emit(0x18); break;
                    case TK_LE:  code.emit(0x19); break;
                    case TK_GT:  code.emit(0x1A); break;
                    case TK_GE:  code.emit(0x1B); break;
                    default: throw runtime_error("内部错误: 未知比较运算符");
                }
            } else {
                switch (op) {
                    case TK_EQ:  code.emit(0x10); break;
                    case TK_NEQ: code.emit(0x11); break;
                    case TK_LT:  code.emit(0x12); break;
                    case TK_LE:  code.emit(0x13); break;
                    case TK_GT:  code.emit(0x14); break;
                    case TK_GE:  code.emit(0x15); break;
                    default: throw runtime_error("内部错误: 未知比较运算符");
                }
            }
            return false;
        }
        return ld;
    }

    bool addsub(bool needDouble) {
        bool ld = muldiv(needDouble);
        if (needDouble && !ld) { code.emit(0x0E); ld = true; }
        while (cur.type == TK_PLUS || cur.type == TK_MINUS) {
            int op = cur.type; advance();
            bool rd = muldiv(ld || needDouble);
            if ((ld || needDouble) && !rd) { code.emit(0x0E); rd = true; }
            bool res = ld || rd;
            code.emit(res ? (op == TK_PLUS ? 0x0A : 0x0B)
                          : (op == TK_PLUS ? 0x06 : 0x07));
            ld = res;
        }
        return ld;
    }

    bool muldiv(bool needDouble) {
        bool ld = unary(false);
        while (cur.type == TK_MUL || cur.type == TK_DIV || cur.type == TK_MOD) {
            int op = cur.type; advance();
            if (op == TK_DIV) {
                if (!ld) { code.emit(0x0E); ld = true; }
                bool rd = unary(true);
                if (!rd) { code.emit(0x0E); }
                code.emit(0x0D);
                ld = true;
            } else if (op == TK_MOD) {
                if (ld) throw runtime_error("取模 '%' 的左操作数为浮点（请使用整数）");
                bool rd = unary(false);
                if (rd) throw runtime_error("取模 '%' 的右操作数为浮点（请使用整数）");
                code.emit(0x0F);
                ld = false;
            } else { // MUL
                if ((ld || needDouble) && !ld) { code.emit(0x0E); ld = true; }
                bool rd = unary(ld || needDouble);
                if ((ld || needDouble) && !rd) { code.emit(0x0E); rd = true; }
                bool res = ld || rd;
                code.emit(res ? 0x0C : 0x08);
                ld = res;
            }
        }
        if (needDouble && !ld) { code.emit(0x0E); ld = true; }
        return ld;
    }

    bool unary(bool ctx) {
        if (cur.type == TK_MINUS) {
            advance();
            bool d = unary(ctx);
            if (!d) { code.emit(0x0E); d = true; }
            // 0 - x 实现一元负号
            code.emit(0x01); code.u16(cp.addInt(0));
            code.emit(d ? 0x0A : 0x06); // 0 + (-x) 取反 → 用 ISUB/FSUB: 0 - x
            return d;
        }
        return primary();
    }

    bool primary() {
        if (cur.type == TK_NUM) {
            if (cur.isFloat) {
                code.emit(0x01); code.u16(cp.addDouble(cur.num));
                advance(); return true;
            } else {
                code.emit(0x01); code.u16(cp.addInt((int32_t)cur.num));
                advance(); return false;
            }
        }
        if (cur.type == TK_STR) {
            code.emit(0x01); code.u16(cp.addString(cur.text));
            advance(); return false;
        }
        if (cur.type == TK_IDENT) {
            string name = cur.text; advance();
            if (cur.type == TK_LPAREN) {
                // 函数调用（表达式上下文也允许）
                advance();
                uint8_t argc = 0;
                if (cur.type != TK_RPAREN) {
                    do {
                        expr(false); argc++;
                        if (cur.type == TK_COMMA) advance();
                        else break;
                    } while (true);
                }
                expect(TK_RPAREN, ")");
                // funcId: 0=pr, 1=inp, 2=prln
                uint8_t funcId = (name == "inp") ? 1 : (name == "prln" ? 2 : 0);
                code.emit(0x02); code.u8(funcId); code.u8(argc);
                return false; // 调用结果当 int 处理（简化）
            }
            // 变量引用
            uint16_t slot = syms.alloc(name);
            code.emit(0x05); code.u16(slot);
            return false;
        }
        if (cur.type == TK_LPAREN) {
            advance();
            bool d = compare(false);
            expect(TK_RPAREN, ")");
            return d;
        }
        throw runtime_error("无法解析的表达式，得到 '" + cur.text + "'");
    }

    // =========================================================================
    //  语句编译
    // =========================================================================
    void program() {
        // 支持可选的顶部全局语句，然后是 main { ... }
        // 也允许整个程序就是 main { ... }，或全局语句序列
        while (cur.type != TK_EOF) {
            if (cur.type == TK_IDENT && cur.text == "main") {
                advance();
                block(); // main 的 { ... }
            } else {
                stmt();
            }
        }
    }

    void stmt() {
        if (cur.type == TK_IF)      { ifStmt(); return; }
        if (cur.type == TK_WHILE)   { whileStmt(); return; }
        if (cur.type == TK_BREAK)   { breakStmt(); return; }
        if (cur.type == TK_CONTINUE){ continueStmt(); return; }
        if (cur.type == TK_LBRACE)  { block(); return; }
        // 表达式语句：赋值 / 复合赋值 / 函数调用
        if (cur.type == TK_IDENT) {
            string name = cur.text;
            // 通过 peek 判断后面是否为赋值（= += -= *= /=）
            if (isAssignmentPeek()) {
                assignStmt(name);
                return;
            }
            // 否则视为函数调用语句（如 pr(...) / inp(...)）
            string fname = name;
            advance(); // 吃掉函数名
            if (cur.type == TK_LPAREN) {
                advance();
                uint8_t argc = 0;
                if (cur.type != TK_RPAREN) {
                    do {
                        expr(false); argc++;
                        if (cur.type == TK_COMMA) advance();
                        else break;
                    } while (true);
                }
                expect(TK_RPAREN, ")");
                uint8_t funcId = (fname == "inp") ? 1 : (fname == "prln" ? 2 : 0);
                code.emit(0x02); code.u8(funcId); code.u8(argc);
                return;
            }
            throw runtime_error("未预期的标识符 '" + name + "'");
        }
        throw runtime_error("无法解析的语句，得到 '" + cur.text + "'");
    }

    // 安全的 peek：通过临时 Lexer 复制
    Token peekToken() {
        Lexer tmp = L;
        return tmp.next();
    }

    bool isAssignmentPeek() {
        if (cur.type != TK_IDENT) return false;
        Token nxt = peekToken();
        return nxt.type == TK_ASSIGN || nxt.type == TK_ADDEQ || nxt.type == TK_SUBEQ
            || nxt.type == TK_MULEQ || nxt.type == TK_DIVEQ;
    }

    void assignStmt(const string& name) {
        uint16_t slot = syms.alloc(name);
        // isAssignmentPeek 只 peek 未 consume，故 cur 仍是 IDENT
        advance(); // 吃掉 IDENT → 现在 cur 指向运算符

        if (cur.type == TK_ASSIGN) {
            advance();
            expr(false);
            code.emit(0x04); code.u16(slot);
            return;
        }

        // 复合赋值 += -= *= /=
        if (cur.type >= TK_ADDEQ && cur.type <= TK_DIVEQ) {
            TK op = cur.type; advance();
            // 先加载变量当前值
            code.emit(0x05); code.u16(slot);
            bool ld = false; // 加载的是 int（简化：变量按 int 处理）
            bool rd = expr(false);
            // 生成运算指令（统一用整数运算，与变量 int 语义一致）
            bool isDiv = (op == TK_DIVEQ);
            if (isDiv) {
                // 除法：若右侧为 double 或需提升
                if (rd) { /* 右侧已是 double */ }
                bool isDouble = ld || rd;
                if (isDouble) {
                    if (!ld) code.emit(0x0E);
                    if (!rd) code.emit(0x0E);
                    code.emit(0x0D); // FDIV
                } else {
                    code.emit(0x09); // IDIV
                }
            } else {
                // += -= *=
                uint8_t arith;
                switch (op) {
                    case TK_ADDEQ: arith = 0x06; break; // IADD
                    case TK_SUBEQ: arith = 0x07; break; // ISUB
                    case TK_MULEQ: arith = 0x08; break; // IMUL
                    default: throw runtime_error("内部错误");
                }
                // 若右侧为 double，提升左侧为 double 并使用浮点运算
                if (rd) {
                    if (!ld) code.emit(0x0E);
                    // 浮点版本：+= → FADD, -= → FSUB, *= → FMUL
                    switch (op) {
                        case TK_ADDEQ: code.emit(0x0A); break;
                        case TK_SUBEQ: code.emit(0x0B); break;
                        case TK_MULEQ: code.emit(0x0C); break;
                        default: break;
                    }
                } else {
                    code.emit(arith);
                }
            }
            code.emit(0x04); code.u16(slot);
            return;
        }
        throw runtime_error("内部错误: 非赋值语句");
    }

    void block() {
        expect(TK_LBRACE, "{");
        while (cur.type != TK_RBRACE && cur.type != TK_EOF) {
            stmt();
        }
        expect(TK_RBRACE, "}");
    }

    // =========================================================================
    //  if / else
    // =========================================================================
    void ifStmt() {
        expect(TK_IF, "if");
        expect(TK_LPAREN, "(");
        expr(false); // 结果 bool 在栈顶
        expect(TK_RPAREN, ")");

        code.emit(0x2A);                    // JZ opcode
        size_t jzPos = placeholderU16();     // JZ 操作数占位（假则跳到 else 或结束）

        stmt(); // then 块

        if (cur.type == TK_ELSE) {
            advance();
            code.emit(0x2B);                    // JMP opcode
            size_t jmpPos = placeholderU16();    // then 结束后 JMP 到 if 结束
            size_t elseStart = code.code.size();
            patchU16(jzPos, jmpOffset(jzPos, elseStart)); // JZ → else 开始
            stmt(); // else 块
            size_t endPos = code.code.size();
            patchU16(jmpPos, jmpOffset(jmpPos, endPos));  // JMP → if 结束
        } else {
            size_t endPos = code.code.size();
            patchU16(jzPos, jmpOffset(jzPos, endPos));    // JZ → if 结束
        }
    }

    // =========================================================================
    //  while 循环
    // =========================================================================
    void whileStmt() {
        expect(TK_WHILE, "while");
        expect(TK_LPAREN, "(");

        size_t condStart = code.code.size(); // 条件计算起点（continue 跳回这里）

        expr(false); // 计算条件 → bool 栈顶
        expect(TK_RPAREN, ")");

        code.emit(0x2A);                    // JZ opcode
        size_t jzPos = placeholderU16();     // JZ 操作数占位（假 → 循环结束）

        loopStack.emplace_back();
        loopStack.back().startPos = condStart;

        stmt(); // 循环体

        // 循环体结束后 → JMP 回条件处
        code.emit(0x2B);                    // JMP opcode
        size_t jmpBackPos = placeholderU16();
        size_t loopEnd = code.code.size();
        patchU16(jmpBackPos, jmpOffset(jmpBackPos, condStart));

        // 回填 JZ（条件为假 → 循环结束）
        patchU16(jzPos, jmpOffset(jzPos, loopEnd));

        // 回填本循环内所有 break / continue
        LoopCtx& ctx = loopStack.back();
        for (size_t pp : ctx.breakPatches) {
            patchU16(pp, jmpOffset(pp, loopEnd));
        }
        for (size_t pp : ctx.contPatches) {
            patchU16(pp, jmpOffset(pp, condStart));
        }
        loopStack.pop_back();
    }

    void breakStmt() {
        if (loopStack.empty()) throw runtime_error("break 必须在循环内使用");
        expect(TK_BREAK, "break");
        // 占位：稍后回填为 JMP 到循环结束
        code.emit(0x2C); // BREAK 占位 opcode（链接器回填为 JMP u16）
        size_t ph = placeholderU16();
        loopStack.back().breakPatches.push_back(ph);
    }

    void continueStmt() {
        if (loopStack.empty()) throw runtime_error("continue 必须在循环内使用");
        expect(TK_CONTINUE, "continue");
        code.emit(0x2D); // CONTINUE 占位 opcode（链接器回填为 JMP u16）
        size_t ph = placeholderU16();
        loopStack.back().contPatches.push_back(ph);
    }
};

// =============================================================================
//  第二遍扫描：把 BREAK/CONTINUE 占位 opcode 替换为真实 JMP
// =============================================================================
//  BREAK(0x2C) / CONTINUE(0x2D) 是「逻辑指令」，链接阶段根据实际跳转目标
//  统一重写为 JMP(0x2B) + u16 偏移。虚拟机只需识别 JMP/JZ。
static void linkBreaks(vector<uint8_t>& code) {
    for (size_t i = 0; i < code.size(); i++) {
        if (code[i] == 0x2C || code[i] == 0x2D) {
            code[i] = 0x2B; // 统一改为 JMP
        }
    }
}

// =============================================================================
//  主流程
// =============================================================================
int main(int argc, char** argv) {
    string path, outPath;
    if (argc > 1) {
        path = argv[1];
        if (argc > 2) outPath = argv[2];
    } else {
        cout << "Ae 编译器 (aec) - 输入源文件路径: ";
        getline(cin, path);
        if (path.empty()) { cerr << "错误: 未提供输入文件" << endl; return 1; }
    }
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
        path = path.substr(1, path.size() - 2);
    if (outPath.empty()) {
        outPath = path;
        if (outPath.size() > 3 && outPath.substr(outPath.size()-3) == ".ae")
            outPath = outPath.substr(0, outPath.size()-3);
        outPath += ".aeo";
    }

    try {
        ifstream f(path);
        if (!f) throw runtime_error("无法打开源文件: " + path);
        string src((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());

        Compiler C(src);
        C.program();

        // 链接：把 BREAK/CONTINUE 占位重写为 JMP
        linkBreaks(C.code.code);

        // 追加 HALT
        C.code.emit(0x03);

        // 写 .aeo
        vector<uint8_t> out;
        out.insert(out.end(), {'A','e','B','c'});
        out.push_back(1); out.push_back(0); // major, minor
        out.push_back(0); out.push_back(0); // flags
        C.cp.write(out);   // 常量池（含 count）
        C.code.write(out); // 代码（含 length）

        ofstream of(outPath, ios::binary);
        of.write((const char*)out.data(), out.size());
        cout << "✓ 编译成功: " << path << " → " << outPath << endl;
        cout << "  (常量池 " << C.cp.pool.size() << " 项, 代码 " << C.code.code.size() << " 字节)" << endl;
    } catch (const exception& e) {
        cerr << "✗ 编译错误: " << e.what() << endl;
        return 1;
    }
    return 0;
}