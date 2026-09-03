// =============================================================================
//  Ae 编译器 (aec = Ae Compiler)  ——  支持自定义函数 + 局部变量 + 递归
//                                      + 字符串拼接 + len/str/int/float
// -----------------------------------------------------------------------------
//  读取 .ae 源文件 → 编译为 .aeo 字节码
//
//  ▌新增语法：
//      字符串拼接:  s = "Hello " + name        // + 号，任一操作数为字符串即拼接
//      长度:        len("abc") → 3              // len(str) 或 len(int)
//      类型转换:    str(123) → "123"            // 任意 → 字符串
//                  int("42") → 42              // 任意 → 整数
//                  float("3.14") → 3.14        // 任意 → 浮点
//      字符串比较:  "abc" == "xyz"             // 直接用 == / != / < / > 等
//
//  ▌内置函数 ID 分配：
//      0 = pr, 1 = inp, 2 = prln, 3 = len, 4 = str, 5 = int, 6 = float
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
//  【常量池】 constPoolCount 条目
//      tag (1 byte) + length (4 byte, 大端) + data
//      tag: 0x01 UTF-8  |  0x02 int32  |  0x03 double
//
//  【字节码流】 codeLength 字节
//      OpCode  助记符          操作数                     语义
//      0x00    NOP             -                         无操作
//      0x01    LOAD_CONST     u16(idx)                   栈压入常量池[idx]
//      0x02    CALL           u8(funcId) u8(argc)        调用内置函数
//      0x03    HALT            -                         停机
//      0x04    STORE          u16(slot)                  栈顶 → 全局变量[slot]
//      0x05    LOAD_VAR       u16(slot)                  全局变量[slot] → 栈顶
//      0x06    IADD            -                         (a+b) int
//      0x07    ISUB            -                         (a-b) int
//      0x08    IMUL            -                         (a*b) int
//      0x09    IDIV            -                         (a/b) int
//      0x0A    FADD            -                         浮点加 / 字符串拼接
//      0x0B    FSUB            -                         浮点减
//      0x0C    FMUL            -                         浮点乘
//      0x0D    FDIV            -                         浮点除
//      0x0E    ITOD            -                         int64 → double
//      0x0F    IMOD            -                         (a%b) int
//      0x10    IEQ             -                         (a==b) int/bool/str
//      0x11    INEQ            -                         (a!=b)
//      0x12    ILT             -                         (a<b)
//      0x13    ILE             -                         (a<=b)
//      0x14    IGT             -                         (a>b)
//      0x15    IGE             -                         (a>=b)
//      0x16    FCMP_EQ         -                         浮点 ==
//      0x17    FCMP_NE         -                         浮点 !=
//      0x18    FCMP_LT         -                         浮点 <
//      0x19    FCMP_LE         -                         浮点 <=
//      0x1A    FCMP_GT         -                         浮点 >
//      0x1B    FCMP_GE         -                         浮点 >=
//      0x2A    JZ             u16(offset)                栈顶 false/0 时跳转
//      0x2B    JMP            u16(offset)                无条件跳转
//      0x30    LOCAL_STORE    u16(slot)                  栈顶 → 局部变量[slot]
//      0x31    LOCAL_LOAD     u16(slot)                  局部变量[slot] → 栈顶
//      0x40    FUNC_DEF       u16(funcId) u16(paramCount)
//                          u16(localCount) u32(codeSize)  函数定义
//      0x41    CALL_FUNC      u16(funcId) u8(argc)       调用用户函数
//      0x42    RET             -                         从函数返回
//      0x43    POP             -                         丢弃栈顶（语句级调用丢弃返回值）
// =============================================================================

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

using namespace std;

// =============================================================================
//  字节码写入辅助（大端）
// =============================================================================
static void writeU16(vector<uint8_t>& v, uint16_t val) {
    v.push_back(static_cast<uint8_t>((val >> 8) & 0xFF)); v.push_back(static_cast<uint8_t>(val & 0xFF));
}
static void writeU32(vector<uint8_t>& v, uint32_t val) {
    v.push_back(static_cast<uint8_t>((val >> 24) & 0xFF)); v.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((val >> 8) & 0xFF)); v.push_back(static_cast<uint8_t>(val & 0xFF));
}

// =============================================================================
//  常量池
// =============================================================================
struct Const {
    enum { UTF8 = 1, INT32 = 2, DOUBLE = 3 } tag;
    string  s;
    int32_t i;
    double  d;
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
    void u32(uint32_t v){ writeU32(code, v); }
    size_t size() const { return code.size(); }
    void patchU16At(size_t pos, uint16_t val) {
        code[pos]   = static_cast<uint8_t>((val >> 8) & 0xFF);
        code[pos+1] = static_cast<uint8_t>(val & 0xFF);
    }
    void write(vector<uint8_t>& out) const {
        writeU32(out, (uint32_t)code.size());
        out.insert(out.end(), code.begin(), code.end());
    }
};

// =============================================================================
//  符号表
// =============================================================================
struct VarInfo {
    enum Kind { GLOBAL, LOCAL } kind;
    uint16_t slot;
};

class SymbolTable {
public:
    map<string, uint16_t> globals;
    uint16_t globalCount = 0;

    map<string, uint16_t> locals;
    uint16_t localCount = 0;

    map<string, uint16_t> functions;       // name → funcId
    map<uint16_t, uint16_t> funcParamCounts; // funcId → paramCount
    set<uint16_t> userFuncIds;

    bool inFunction = false;

    void resetForFunction() {
        locals.clear();
        localCount = 0;
    }

    VarInfo resolve(const string& name) const {
        if (inFunction) {
            auto it = locals.find(name);
            if (it != locals.end()) return {VarInfo::LOCAL, it->second};
        }
        auto it = globals.find(name);
        if (it != globals.end()) return {VarInfo::GLOBAL, it->second};
        return {VarInfo::GLOBAL, (uint16_t)-1};
    }

    uint16_t allocGlobal(const string& name) {
        auto it = globals.find(name);
        if (it != globals.end()) return it->second;
        uint16_t slot = globalCount++;
        globals[name] = slot;
        return slot;
    }

    uint16_t allocLocal(const string& name) {
        auto it = locals.find(name);
        if (it != locals.end()) return it->second;
        uint16_t slot = localCount++;
        locals[name] = slot;
        return slot;
    }

    uint16_t allocParam(const string& name) {
        return allocLocal(name);
    }
};

// =============================================================================
//  词法分析
// =============================================================================
enum TK {
    TK_EOF, TK_NUM, TK_STR, TK_IDENT, TK_BOOL, TK_ASSIGN,
    TK_PLUS, TK_MINUS, TK_MUL, TK_DIV, TK_MOD,
    TK_EQ, TK_NEQ, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_ADDEQ, TK_SUBEQ, TK_MULEQ, TK_DIVEQ,
    TK_FUNC, TK_IF, TK_ELSE, TK_WHILE, TK_BREAK, TK_CONTINUE,
    TK_RETURN, TK_LOCAL,
    // 内置函数（len/str/int/float）统一作为 TK_IDENT，primary() 按名字识别
    TK_LPAREN, TK_RPAREN, TK_COMMA, TK_LBRACE, TK_RBRACE
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
    const string& source() const { return src; }
    void reset(const string& s) { src = s; pos = 0; }
    void skip() {
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n' || src[pos] == '\r')) pos++;
    }
    Token next() {
        skip();
        if (pos >= src.size()) return {TK_EOF};
        char c = src[pos];

        if (c == '/' && pos + 1 < src.size() && src[pos+1] == '/') {
            while (pos < src.size() && src[pos] != '\n') pos++;
            return next();
        }
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
        if (isalpha(c) || c == '_') {
            string id; while (pos < src.size() && (isalnum(src[pos]) || src[pos] == '_')) id += src[pos++];
            // 关键字
            if (id == "if")       return {TK_IF, id};
            if (id == "else")     return {TK_ELSE, id};
            if (id == "while")    return {TK_WHILE, id};
            if (id == "break")    return {TK_BREAK, id};
            if (id == "continue") return {TK_CONTINUE, id};
            if (id == "return")   return {TK_RETURN, id};
            if (id == "local")    return {TK_LOCAL, id};
            if (id == "func")     return {TK_FUNC, id};
            if (id == "true")     return {TK_BOOL, id, 1, false};
            if (id == "false")    return {TK_BOOL, id, 0, false};
            // 内置函数（返回 TK_IDENT，primary() 里按名字识别为内置调用）
            if (id == "len")      return {TK_IDENT, id};
            if (id == "str")      return {TK_IDENT, id};
            if (id == "int")      return {TK_IDENT, id};
            if (id == "float")    return {TK_IDENT, id};
            // 运行时内置（保留为普通标识符）
            if (id == "pr")       return {TK_IDENT, id};
            if (id == "prln")     return {TK_IDENT, id};
            if (id == "inp")      return {TK_IDENT, id};
            if (id == "main")     return {TK_IDENT, id};
            return {TK_IDENT, id, 0, false};
        }
        pos++;
        if (c == '=' && pos < src.size() && src[pos] == '=') { pos++; return {TK_EQ, "==", 0, false}; }
        if (c == '!' && pos < src.size() && src[pos] == '=') { pos++; return {TK_NEQ, "!=", 0, false}; }
        if (c == '<' && pos < src.size() && src[pos] == '=') { pos++; return {TK_LE, "<=", 0, false}; }
        if (c == '>' && pos < src.size() && src[pos] == '=') { pos++; return {TK_GE, ">=", 0, false}; }
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
            default:  return {TK_EOF, string(1, c)};
        }
    }
};

// =============================================================================
//  编译器
// =============================================================================
class Compiler {
public:
    Lexer    L; Token cur;
    ConstantPool cp;
    SymbolTable  syms;

    CodeBuf  output;
    CodeBuf* currentCode = &output;
    vector<vector<uint8_t>> funcDefs;

    // 内置函数 ID
    static const uint8_t BID_pr   = 0;
    static const uint8_t BID_inp  = 1;
    static const uint8_t BID_prln = 2;
    static const uint8_t BID_len  = 3;
    static const uint8_t BID_str  = 4;
    static const uint8_t BID_int  = 5;
    static const uint8_t BID_float = 6;

    // 内置函数名 → ID 映射（含关键字型内置）
    uint8_t builtinId(const string& name) const {
        if (name == "pr")     return BID_pr;
        if (name == "inp")    return BID_inp;
        if (name == "prln")   return BID_prln;
        if (name == "len")    return BID_len;
        if (name == "str")    return BID_str;
        if (name == "int")    return BID_int;
        if (name == "float")  return BID_float;
        return 0xFF;
    }

    uint16_t nextUserFuncId = 7; // 0-6 留给内置

    bool declOnly = false;

    void skipBlock() {
        if (cur.type != TK_LBRACE) return;
        int depth = 1;
        advance();
        while (cur.type != TK_EOF && depth > 0) {
            if (cur.type == TK_LBRACE) depth++;
            else if (cur.type == TK_RBRACE) depth--;
            if (depth > 0) advance();
        }
        if (cur.type == TK_RBRACE) advance();
    }

    void pc_reset() { L.reset(L.source()); advance(); }

    struct LoopCtx {
        size_t startPos;
        vector<size_t> breakPatches;
        vector<size_t> contPatches;
    };
    vector<LoopCtx> loopStack;

    Compiler(const string& src) : L(src) { advance(); }
    void advance() { cur = L.next(); }

    void expect(TK t, const string& m) {
        if (cur.type != t) throw runtime_error("语法错误: 期望 " + m + "，得到 '" + cur.text + "'");
        advance();
    }

    size_t placeholderU16() { currentCode->u16(0x0000); return currentCode->size() - 2; }
    void patchU16(size_t pos, uint16_t val) { currentCode->patchU16At(pos, val); }

    uint16_t jmpOffset(size_t placeholderPos, size_t targetPos) {
        int64_t off = (int64_t)targetPos - (int64_t)(placeholderPos + 2);
        if (off < -32768 || off > 32767) throw runtime_error("跳转跨度过大（超出 int16）");
        return (uint16_t)(int16_t)off;
    }

    // =========================================================================
    //  顶层
    // =========================================================================
    void program() {
        declOnly = true;
        while (cur.type != TK_EOF) {
            if (cur.type == TK_FUNC) funcDef();
            else advance();
        }
        declOnly = false;
        pc_reset();
        while (cur.type != TK_EOF) {
            if (cur.type == TK_FUNC) {
                funcDef();
            } else if (cur.type == TK_IDENT && cur.text == "main") {
                advance();
                expect(TK_LBRACE, "{");
                while (cur.type != TK_RBRACE && cur.type != TK_EOF) {
                    stmt();
                }
                expect(TK_RBRACE, "}");
            } else {
                stmt();
            }
        }
        for (auto& fdef : funcDefs) {
            for (uint8_t b : fdef) {
                output.emit(b);
            }
        }
        output.emit(0x03); // HALT
    }

    void funcDef() {
        advance(); // 'func'
        if (cur.type != TK_IDENT) throw runtime_error("func 后需要函数名");
        string fname = cur.text;
        advance();

        uint16_t funcId;
        auto it = syms.functions.find(fname);
        if (it != syms.functions.end()) {
            funcId = it->second;
        } else {
            funcId = nextUserFuncId++;
            syms.functions[fname] = funcId;
        }

        expect(TK_LPAREN, "(");

        syms.inFunction = true;
        syms.resetForFunction();

        vector<string> params;
        if (cur.type != TK_RPAREN) {
            if (cur.type != TK_IDENT) throw runtime_error("参数名必须是标识符");
            params.push_back(cur.text);
            syms.allocParam(cur.text);
            advance();
            while (cur.type == TK_COMMA) {
                advance();
                if (cur.type != TK_IDENT) throw runtime_error("参数名必须是标识符");
                params.push_back(cur.text);
                syms.allocParam(cur.text);
                advance();
            }
        }
        syms.funcParamCounts[funcId] = (uint16_t)params.size();
        expect(TK_RPAREN, ")");
        expect(TK_LBRACE, "{");

        CodeBuf bodyBuf;
        CodeBuf* savedCode = currentCode;
        if (declOnly) {
            syms.inFunction = false;
            skipBlock();
            return;
        }
        currentCode = &bodyBuf;

        while (cur.type != TK_RBRACE && cur.type != TK_EOF) {
            stmt();
        }
        expect(TK_RBRACE, "}");

        if (bodyBuf.size() == 0 || bodyBuf.code.back() != 0x42) {
            bodyBuf.emit(0x01);
            bodyBuf.u16(cp.addInt(0));
            bodyBuf.emit(0x42);
        }

        currentCode = savedCode;
        syms.inFunction = false;

        CodeBuf fullDef;
        fullDef.emit(0x40);
        fullDef.u16(funcId);
        fullDef.u16((uint16_t)params.size());
        fullDef.u16(syms.localCount);
        fullDef.u32((uint32_t)bodyBuf.size());
        for (uint8_t b : bodyBuf.code) {
            fullDef.emit(b);
        }

        funcDefs.push_back(fullDef.code);
    }

    // =========================================================================
    //  语句
    // =========================================================================
    void stmt() {
        if (cur.type == TK_IF) {
            ifStmt();
        } else if (cur.type == TK_WHILE) {
            whileStmt();
        } else if (cur.type == TK_BREAK) {
            if (loopStack.empty()) throw runtime_error("break 只能在循环体内使用");
            advance();
            currentCode->emit(0x2B);
            size_t pos = placeholderU16();
            loopStack.back().breakPatches.push_back(pos);
        } else if (cur.type == TK_CONTINUE) {
            if (loopStack.empty()) throw runtime_error("continue 只能在循环体内使用");
            advance();
            currentCode->emit(0x2B);
            size_t pos = placeholderU16();
            loopStack.back().contPatches.push_back(pos);
        } else if (cur.type == TK_RETURN) {
            returnStmt();
        } else if (cur.type == TK_LOCAL) {
            localDecl();
        } else if (cur.type == TK_LBRACE) {
            advance();
            while (cur.type != TK_RBRACE && cur.type != TK_EOF) {
                stmt();
            }
            if (cur.type == TK_RBRACE) advance();
        } else {
            exprStmt();
        }
    }

    void exprStmt() {
        if (cur.type == TK_IDENT) {
            string name = cur.text;
            Lexer peekL = L;
            Token peek = peekL.next();

            if (peek.type == TK_ASSIGN) {
                advance(); // IDENT
                advance(); // =
                compare(false);
                VarInfo vi = syms.resolve(name);
                if (vi.kind == VarInfo::LOCAL) {
                    currentCode->emit(0x30); currentCode->u16(vi.slot);
                } else {
                    if (vi.slot == (uint16_t)-1) vi.slot = syms.allocGlobal(name);
                    currentCode->emit(0x04); currentCode->u16(vi.slot);
                }
                return;
            } else if (peek.type == TK_ADDEQ || peek.type == TK_SUBEQ ||
                       peek.type == TK_MULEQ || peek.type == TK_DIVEQ) {
                advance();
                TK op = cur.type;
                advance();
                VarInfo vi = syms.resolve(name);
                if (vi.kind == VarInfo::LOCAL) {
                    currentCode->emit(0x31); currentCode->u16(vi.slot);
                } else {
                    if (vi.slot == (uint16_t)-1) throw runtime_error("变量 '" + name + "' 未定义");
                    currentCode->emit(0x05); currentCode->u16(vi.slot);
                }
                compare(false);
                switch (op) {
                    case TK_ADDEQ: currentCode->emit(0x0A); break;
                    case TK_SUBEQ: currentCode->emit(0x0B); break;
                    case TK_MULEQ: currentCode->emit(0x0C); break;
                    case TK_DIVEQ: currentCode->emit(0x0D); break;
                    default: break;
                }
                if (vi.kind == VarInfo::LOCAL) {
                    currentCode->emit(0x30); currentCode->u16(vi.slot);
                } else {
                    currentCode->emit(0x04); currentCode->u16(vi.slot);
                }
                return;
            }
            // 函数调用语句（含内置函数）
            if (peek.type == TK_LPAREN) {
                callExpr(true);
                return;
            }
        }

        compare(false);
    }

    void localDecl() {
        advance(); // 'local'
        if (cur.type != TK_IDENT) throw runtime_error("local 后需要变量名");
        string name = cur.text;
        advance();
        expect(TK_ASSIGN, "=");
        compare(false);
        if (!syms.inFunction) throw runtime_error("local 只能在函数内使用");
        uint16_t slot = syms.allocLocal(name);
        currentCode->emit(0x30);
        currentCode->u16(slot);
    }

    void ifStmt() {
        advance(); // 'if'
        expect(TK_LPAREN, "(");
        compare(false);
        expect(TK_RPAREN, ")");

        currentCode->emit(0x2A);
        size_t jzPos = placeholderU16();

        if (cur.type == TK_LBRACE) {
            advance();
            while (cur.type != TK_RBRACE && cur.type != TK_EOF) stmt();
            if (cur.type == TK_RBRACE) advance();
        } else {
            stmt();
        }

        if (cur.type == TK_ELSE) {
            advance();
            currentCode->emit(0x2B);
            size_t jmpPos = placeholderU16();
            size_t elseStart = currentCode->size();
            patchU16(jzPos, jmpOffset(jzPos, elseStart));

            if (cur.type == TK_LBRACE) {
                advance();
                while (cur.type != TK_RBRACE && cur.type != TK_EOF) stmt();
                if (cur.type == TK_RBRACE) advance();
            } else {
                stmt();
            }
            size_t endPos = currentCode->size();
            patchU16(jmpPos, jmpOffset(jmpPos, endPos));
        } else {
            size_t endPos = currentCode->size();
            patchU16(jzPos, jmpOffset(jzPos, endPos));
        }
    }

    void whileStmt() {
        LoopCtx ctx;
        ctx.startPos = currentCode->size();
        loopStack.push_back(ctx);
        size_t loopStart = currentCode->size();

        advance(); // 'while'
        expect(TK_LPAREN, "(");
        compare(false);
        expect(TK_RPAREN, ")");

        currentCode->emit(0x2A);
        size_t jzPos = placeholderU16();

        if (cur.type == TK_LBRACE) {
            advance();
            while (cur.type != TK_RBRACE && cur.type != TK_EOF) stmt();
            if (cur.type == TK_RBRACE) advance();
        } else {
            stmt();
        }

        currentCode->emit(0x2B);
        size_t jmpBackPos = placeholderU16();
        size_t loopEnd = currentCode->size();

        LoopCtx& lc = loopStack.back();
        for (size_t pos : lc.breakPatches) patchU16(pos, jmpOffset(pos, loopEnd));
        for (size_t pos : lc.contPatches) patchU16(pos, jmpOffset(pos, loopStart));
        loopStack.pop_back();

        patchU16(jzPos, jmpOffset(jzPos, loopEnd));
        patchU16(jmpBackPos, jmpOffset(jmpBackPos, loopStart));
    }

    void returnStmt() {
        if (!syms.inFunction) throw runtime_error("return 只能在函数内使用");
        advance(); // 'return'
        if (cur.type != TK_RBRACE && cur.type != TK_EOF) {
            compare(false);
        } else {
            currentCode->emit(0x01);
            currentCode->u16(cp.addInt(0));
        }
        currentCode->emit(0x42);
    }

    // =========================================================================
    //  表达式
    // =========================================================================

    // 结果类型标记：返回 true 表示"结果为 double"，false 表示"结果为 int/str/bool"
    // 但为了实现字符串拼接，我们需要更精细的类型信息，故用 enum
    enum ResType { RT_INT, RT_DOUBLE, RT_STR, RT_BOOL };

    ResType compare(bool /*needDouble*/ = false) {
        ResType lt = addsub();
        if (cur.type >= TK_EQ && cur.type <= TK_GE) {
            TK op = cur.type; advance();
            ResType rt = addsub();
            // 如果任一操作数为字符串 → 用整数比较（IEQ 系列，已支持字符串字典序）
            bool useStringCmp = (lt == RT_STR || rt == RT_STR);
            // 如果任一为 double 且无字符串 → 浮点比较
            bool useDoubleCmp = !useStringCmp && (lt == RT_DOUBLE || rt == RT_DOUBLE);

            if (useStringCmp) {
                // IEQ..IGE 已支持字符串，无需类型转换
                switch (op) {
                    case TK_EQ:  currentCode->emit(0x10); break;
                    case TK_NEQ: currentCode->emit(0x11); break;
                    case TK_LT:  currentCode->emit(0x12); break;
                    case TK_LE:  currentCode->emit(0x13); break;
                    case TK_GT:  currentCode->emit(0x14); break;
                    case TK_GE:  currentCode->emit(0x15); break;
                    default: break;
                }
            } else if (useDoubleCmp) {
                if (lt == RT_INT) currentCode->emit(0x0E);
                if (rt == RT_INT) currentCode->emit(0x0E);
                switch (op) {
                    case TK_EQ:  currentCode->emit(0x16); break;
                    case TK_NEQ: currentCode->emit(0x17); break;
                    case TK_LT:  currentCode->emit(0x18); break;
                    case TK_LE:  currentCode->emit(0x19); break;
                    case TK_GT:  currentCode->emit(0x1A); break;
                    case TK_GE:  currentCode->emit(0x1B); break;
                    default: break;
                }
            } else {
                switch (op) {
                    case TK_EQ:  currentCode->emit(0x10); break;
                    case TK_NEQ: currentCode->emit(0x11); break;
                    case TK_LT:  currentCode->emit(0x12); break;
                    case TK_LE:  currentCode->emit(0x13); break;
                    case TK_GT:  currentCode->emit(0x14); break;
                    case TK_GE:  currentCode->emit(0x15); break;
                    default: break;
                }
            }
            return RT_BOOL;
        }
        return lt;
    }

    ResType addsub() {
        ResType lt = muldiv();
        while (cur.type == TK_PLUS || cur.type == TK_MINUS) {
            TK op = cur.type; advance();
            ResType rt = muldiv();
            // 字符串拼接：任一操作数为字符串 → FADD（运行时处理）
            if (lt == RT_STR || rt == RT_STR) {
                // 如果左操作数不是字符串，需要 ITOD？不，运行时 toString 处理
                // 但需要确保两侧都被当作"值"压栈（它们已经是）
                // FADD 运行时检测字符串 → 拼接
                currentCode->emit(0x0A); // FADD = 字符串拼接
                lt = RT_STR;
            } else if (lt == RT_DOUBLE || rt == RT_DOUBLE) {
                if (lt == RT_INT) currentCode->emit(0x0E);
                if (rt == RT_INT) currentCode->emit(0x0E);
                currentCode->emit(op == TK_PLUS ? 0x0A : 0x0B);
                lt = RT_DOUBLE;
            } else {
                currentCode->emit(op == TK_PLUS ? 0x06 : 0x07);
                lt = RT_INT;
            }
        }
        return lt;
    }

    ResType muldiv() {
        ResType lt = unary();
        while (cur.type == TK_MUL || cur.type == TK_DIV || cur.type == TK_MOD) {
            TK op = cur.type; advance();
            ResType rt = unary();
            if (op == TK_MOD) {
                currentCode->emit(0x0F);
                lt = RT_INT;
            } else if (lt == RT_DOUBLE || rt == RT_DOUBLE) {
                if (lt == RT_INT) currentCode->emit(0x0E);
                if (rt == RT_INT) currentCode->emit(0x0E);
                currentCode->emit(op == TK_MUL ? 0x0C : 0x0D);
                lt = RT_DOUBLE;
            } else {
                currentCode->emit(op == TK_MUL ? 0x08 : 0x09);
                lt = RT_INT;
            }
        }
        return lt;
    }

    ResType unary() {
        if (cur.type == TK_MINUS) {
            advance();
            ResType rt = primary();
            uint16_t tmpSlot = syms.inFunction
                ? syms.allocLocal("__neg_tmp")
                : syms.allocGlobal("__neg_tmp");
            if (rt == RT_DOUBLE) {
                currentCode->emit(syms.inFunction ? 0x30 : 0x04); currentCode->u16(tmpSlot);
                currentCode->emit(0x01); currentCode->u16(cp.addDouble(0.0));
                currentCode->emit(syms.inFunction ? 0x31 : 0x05); currentCode->u16(tmpSlot);
                currentCode->emit(0x0B);
            } else {
                currentCode->emit(syms.inFunction ? 0x30 : 0x04); currentCode->u16(tmpSlot);
                currentCode->emit(0x01); currentCode->u16(cp.addInt(0));
                currentCode->emit(syms.inFunction ? 0x31 : 0x05); currentCode->u16(tmpSlot);
                currentCode->emit(0x07);
            }
            return rt;
        }
        return primary();
    }

    ResType primary() {
        if (cur.type == TK_NUM) {
            if (cur.isFloat) {
                currentCode->emit(0x01);
                currentCode->u16(cp.addDouble(cur.num));
                advance();
                return RT_DOUBLE;
            } else {
                currentCode->emit(0x01);
                currentCode->u16(cp.addInt((int32_t)cur.num));
                advance();
                return RT_INT;
            }
        } else if (cur.type == TK_STR) {
            currentCode->emit(0x01);
            currentCode->u16(cp.addString(cur.text));
            advance();
            return RT_STR;
        } else if (cur.type == TK_BOOL) {
            currentCode->emit(0x01);
            currentCode->u16(cp.addInt(cur.num ? 1 : 0));
            advance();
            return RT_BOOL;
        } else if (cur.type == TK_LPAREN) {
            advance();
            ResType d = compare();
            expect(TK_RPAREN, ")");
            return d;
        } else if (cur.type == TK_IDENT) {
            string name = cur.text;
            Lexer peekL = L;
            Token peek = peekL.next();
            if (peek.type == TK_LPAREN) {
                // 可能是内置函数（含 len/str/int/float）
                uint8_t bid = builtinId(name);
                if (bid != 0xFF) {
                    callBuiltin(bid);
                    // 返回类型：str() 返回字符串，其余返回 int/double
                    if (bid == BID_str) return RT_STR;
                    if (bid == BID_float) return RT_DOUBLE;
                    return RT_INT;
                } else {
                    callUserFunc();
                    return RT_INT; // 简化：用户函数返回类型不精确追踪
                }
            } else {
                VarInfo vi = syms.resolve(name);
                if (vi.kind == VarInfo::LOCAL) {
                    currentCode->emit(0x31);
                    currentCode->u16(vi.slot);
                } else {
                    if (vi.slot == (uint16_t)-1) {
                        vi.slot = syms.allocGlobal(name);
                    }
                    currentCode->emit(0x05);
                    currentCode->u16(vi.slot);
                }
                advance();
                return RT_INT; // 变量类型动态，编译期不精确追踪
            }
        } else {
            throw runtime_error("语法错误: 无法解析 '" + cur.text + "'");
        }
    }

    // 该内置函数是否有"返回值"（语句级调用时需要 POP）
    bool builtinHasReturn(uint8_t bid) const {
        // pr/prln 是语句型，无返回值；其余（inp/len/str/int/float）都有返回值
        return (bid != BID_pr && bid != BID_prln);
    }

    // 调用内置函数（含 len/str/int/float/pr/prln/inp）
    // discardReturn: 是否为语句级调用（无人接收返回值）→ 有返回值时追加 POP
    void callBuiltin(uint8_t bid, bool discardReturn = false) {
        advance(); // func name
        expect(TK_LPAREN, "(");
        uint8_t argc = 0;
        if (cur.type != TK_RPAREN) {
            argc = 1;
            compare(); // 参数表达式（可能返回任意类型）
            while (cur.type == TK_COMMA) {
                advance();
                compare();
                argc++;
            }
        }
        expect(TK_RPAREN, ")");
        currentCode->emit(0x02);
        currentCode->u8(bid);
        currentCode->u8(argc);
        if (discardReturn && builtinHasReturn(bid)) {
            currentCode->emit(0x43); // POP 丢弃返回值
        }
    }

    // 调用用户自定义函数
    // discardReturn: 语句级调用 → 追加 POP（用户函数总有返回值）
    void callUserFunc(bool discardReturn = false) {
        string funcName = cur.text;
        advance();
        expect(TK_LPAREN, "(");
        auto it = syms.functions.find(funcName);
        if (it == syms.functions.end()) throw runtime_error("未定义的函数: " + funcName);
        uint16_t funcId = it->second;
        uint16_t expectedArgs = syms.funcParamCounts[funcId];

        uint8_t argc = 0;
        if (cur.type != TK_RPAREN) {
            argc = 1;
            compare();
            while (cur.type == TK_COMMA) {
                advance();
                compare();
                argc++;
            }
        }
        expect(TK_RPAREN, ")");

        if (argc != (uint8_t)expectedArgs) {
            throw runtime_error("函数 '" + funcName + "' 参数数量不匹配: 期望 " +
                to_string(expectedArgs) + "，得到 " + to_string(argc));
        }

        currentCode->emit(0x41);
        currentCode->u16(funcId);
        currentCode->u8(argc);
        if (discardReturn) currentCode->emit(0x43); // POP
    }

    // callExpr：语句级调用（用于 exprStmt 中的函数调用）
    void callExpr(bool discardReturn = false) {
        string name = cur.text;
        uint8_t bid = builtinId(name);
        if (bid != 0xFF) {
            callBuiltin(bid, discardReturn);
        } else {
            callUserFunc(discardReturn);
        }
    }

    // =========================================================================
    //  编译入口
    // =========================================================================
    void compile() {
        program();
    }

    void save(const string& path) {
        vector<uint8_t> out;
        out.insert(out.end(), {'A', 'e', 'B', 'c'});
        out.push_back(1);
        out.push_back(0);
        out.push_back(0);
        out.push_back(0);

        cp.write(out);
        output.write(out);

        ofstream f(path, ios::binary);
        f.write((const char*)out.data(), static_cast<streamsize>(out.size()));
    }
};

// =============================================================================
//  main
// =============================================================================
int main(int argc, char** argv) {
    string inPath, outPath;
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            string a = argv[i];
            if (a == "-o" && i + 1 < argc) {
                outPath = argv[++i];
            } else if (!inPath.empty() && outPath.empty() && a[0] != '-') {
                outPath = a; // 位置式：第二个非flag参数作为输出
            } else if (inPath.empty()) {
                inPath = a;
            }
        }
    } else {
        cout << "Ae 编译器 (aec) - 输入源文件路径: ";
        getline(cin, inPath);
        if (inPath.empty()) { cerr << "错误: 未提供输入文件" << endl; return 1; }
    }
    if (inPath.size() >= 2 && inPath.front() == '"' && inPath.back() == '"')
        inPath = inPath.substr(1, inPath.size() - 2);

    if (outPath.empty()) {
        outPath = inPath;
        if (outPath.size() > 3 && outPath.substr(outPath.size() - 3) == ".ae")
            outPath = outPath.substr(0, outPath.size() - 3);
        outPath += ".aeo";
    }

    try {
        ifstream f(inPath);
        if (!f) throw runtime_error("无法打开源文件: " + inPath);
        string src((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());

        Compiler C(src);
        C.compile();
        C.save(outPath);

        cout << "✓ 编译成功: " << inPath << " → " << outPath << endl;
        cout << "  常量池: " << C.cp.pool.size() << " 项" << endl;
        cout << "  全局变量: " << C.syms.globalCount << " 个" << endl;
        cout << "  函数: " << C.funcDefs.size() << " 个" << endl;
    } catch (const exception& e) {
        cerr << "✗ 编译错误: " << e.what() << endl;
        return 1;
    }
    return 0;
}