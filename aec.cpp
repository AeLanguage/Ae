// =============================================================================
//  Ae 编译器 (aec = Ae Compiler)  ——  支持自定义函数 + 局部变量 + 递归
// -----------------------------------------------------------------------------
//  读取 .ae 源文件 → 编译为 .aeo 字节码
//
//  ▌语法扩展：自定义函数
//      func name(param1, param2, ...) {
//          local x = 10       // 局部变量
//          return x           // 返回值
//      }
//      - 函数只能定义在 main 之外（顶部）
//      - 参数和 local 声明都是局部变量（隔离于全局）
//      - 支持递归调用
//      - return 可返回任意类型，无 return 默认返回 0
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
//      0x0A    FADD            -                         浮点加
//      0x0B    FSUB            -                         浮点减
//      0x0C    FMUL            -                         浮点乘
//      0x0D    FDIV            -                         浮点除
//      0x0E    ITOD            -                         int64 → double
//      0x0F    IMOD            -                         (a%b) int
//      0x10    IEQ             -                         (a==b) bool
//      0x11    INEQ            -                         (a!=b) bool
//      0x12    ILT             -                         (a<b) bool
//      0x13    ILE             -                         (a<=b) bool
//      0x14    IGT             -                         (a>b) bool
//      0x15    IGE             -                         (a>=b) bool
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
        code[pos]   = (val >> 8) & 0xFF;
        code[pos+1] = val & 0xFF;
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
    set<uint16_t> userFuncIds;             // 用户定义的函数 ID 集合

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

    // 最终输出：完整的字节码（全局代码 + 函数定义）
    CodeBuf  output;

    // 当前代码缓冲区（指向 output 或 funcBodyBuf）
    CodeBuf* currentCode = &output;

    // 函数定义编译结果（在顶层代码之后追加）
    vector<vector<uint8_t>> funcDefs;

    // 内置函数 ID
    static const uint8_t BID_pr   = 0;
    static const uint8_t BID_inp  = 1;
    static const uint8_t BID_prln = 2;

    // 下一个用户函数 ID（从 3 开始，0/1/2 留给内置）
    uint16_t nextUserFuncId = 3;

    // 循环上下文
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
        while (cur.type != TK_EOF) {
            if (cur.type == TK_FUNC) {
                funcDef();
            } else if (cur.type == TK_IDENT && cur.text == "main") {
                advance(); // 'main'
                expect(TK_LBRACE, "{");
                while (cur.type != TK_RBRACE && cur.type != TK_EOF) {
                    stmt();
                }
                expect(TK_RBRACE, "}");
            } else {
                stmt();
            }
        }
        // 追加所有函数定义到输出末尾
        for (auto& fdef : funcDefs) {
            for (uint8_t b : fdef) {
                output.emit(b);
            }
        }
        output.emit(0x03); // HALT（确保顶层有终止）
    }

    // 解析函数定义
    void funcDef() {
        advance(); // 'func'
        if (cur.type != TK_IDENT) throw runtime_error("func 后需要函数名");
        string fname = cur.text;
        advance();

        // 记录函数
        uint16_t funcId = nextUserFuncId++;
        syms.functions[fname] = funcId;

        expect(TK_LPAREN, "(");

        // 进入函数作用域
        syms.inFunction = true;
        syms.resetForFunction();

        // 参数列表
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

        // 创建函数体的独立缓冲区
        CodeBuf bodyBuf;
        CodeBuf* savedCode = currentCode;
        currentCode = &bodyBuf;

        // 编译函数体
        while (cur.type != TK_RBRACE && cur.type != TK_EOF) {
            stmt();
        }
        expect(TK_RBRACE, "}");

        // 如果函数体末尾没有 RET，自动添加
        if (bodyBuf.size() == 0 || bodyBuf.code.back() != 0x42) {
            bodyBuf.emit(0x01); // LOAD_CONST 0
            bodyBuf.u16(cp.addInt(0));
            bodyBuf.emit(0x42); // RET
        }

        // 恢复
        currentCode = savedCode;
        syms.inFunction = false;

        // 组装 FUNC_DEF 字节码
        CodeBuf fullDef;
        fullDef.emit(0x40);                    // FUNC_DEF
        fullDef.u16(funcId);
        fullDef.u16((uint16_t)params.size());  // paramCount
        fullDef.u16(syms.localCount);           // localCount（含参数 + 局部变量）
        fullDef.u32((uint32_t)bodyBuf.size());  // codeSize
        // 函数体
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
            advance();
            currentCode->emit(0x2B); // JMP
            size_t pos = placeholderU16();
            loopStack.back().breakPatches.push_back(pos);
        } else if (cur.type == TK_CONTINUE) {
            advance();
            currentCode->emit(0x2B); // JMP
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
        // 赋值语句：IDENT = expr
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
                advance(); // IDENT
                TK op = cur.type;
                advance(); // 运算符
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
            // 函数调用语句（如 prln(...) 或 foo() 单独成句）
            if (peek.type == TK_LPAREN) {
                callExpr(true);  // 语句级：丢弃返回值
                return;
            }
        }

        // 表达式语句（兜底）
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
        compare(false); // 条件 → 栈顶 bool
        expect(TK_RPAREN, ")");

        // JZ 指令：opcode + u16 偏移
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
            // JMP 跳过 else
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

        // JZ 跳出循环
        currentCode->emit(0x2A);
        size_t jzPos = placeholderU16();

        if (cur.type == TK_LBRACE) {
            advance();
            while (cur.type != TK_RBRACE && cur.type != TK_EOF) stmt();
            if (cur.type == TK_RBRACE) advance();
        } else {
            stmt();
        }

        // JMP 回到条件
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
            compare(false); // 返回值
        } else {
            currentCode->emit(0x01); // LOAD_CONST 0
            currentCode->u16(cp.addInt(0));
        }
        currentCode->emit(0x42); // RET
    }

    // =========================================================================
    //  表达式
    // =========================================================================
    bool compare(bool needDouble) {
        bool ld = addsub(needDouble);
        if (cur.type >= TK_EQ && cur.type <= TK_GE) {
            TK op = cur.type; advance();
            bool rd = addsub(needDouble);
            bool isDouble = ld || rd;
            if (isDouble) {
                if (!ld) currentCode->emit(0x0E);
                if (!rd) currentCode->emit(0x0E);
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
            return false;
        }
        return ld;
    }

    bool addsub(bool needDouble) {
        bool ld = muldiv(needDouble);
        while (cur.type == TK_PLUS || cur.type == TK_MINUS) {
            TK op = cur.type; advance();
            bool rd = muldiv(needDouble || ld);
            bool isDouble = ld || rd;
            if (isDouble) {
                if (!ld) currentCode->emit(0x0E);
                if (!rd) currentCode->emit(0x0E);
                currentCode->emit(op == TK_PLUS ? 0x0A : 0x0B);
            } else {
                currentCode->emit(op == TK_PLUS ? 0x06 : 0x07);
            }
            ld = isDouble;
        }
        return ld;
    }

    bool muldiv(bool needDouble) {
        bool ld = unary(needDouble);
        while (cur.type == TK_MUL || cur.type == TK_DIV || cur.type == TK_MOD) {
            TK op = cur.type; advance();
            bool rd = unary(needDouble || ld);
            bool isDouble = ld || rd;
            if (op == TK_MOD) {
                currentCode->emit(0x0F);
                ld = false;
            } else if (isDouble) {
                if (!ld) currentCode->emit(0x0E);
                if (!rd) currentCode->emit(0x0E);
                currentCode->emit(op == TK_MUL ? 0x0C : 0x0D);
                ld = true;
            } else {
                currentCode->emit(op == TK_MUL ? 0x08 : 0x09);
                ld = false;
            }
        }
        return ld;
    }

    bool unary(bool needDouble) {
        if (cur.type == TK_MINUS) {
            advance();
            bool d = primary(needDouble);
            // 生成 0 - x：需要交换栈顺序
            // 策略：先把 x 存到临时全局槽，再加载 0，再加载 x，再 ISUB
            // 简化：用一个固定临时槽
            // 更好的方案：直接在 primary 结果上取负
            // 由于栈式 VM：x 已在栈顶，我们生成：DUP → STORE tmp → LOAD_CONST 0 → LOAD_VAR tmp → ISUB
            // 但我们没有 DUP 指令... 用两次加载代替
            // 最简单：把 x 存到临时变量，然后 0 - tmp
            // 使用全局槽 0xFFFF 作为临时... 不安全
            // 实际方案：在 primary 之前预留，这里用一个 hack：
            // 生成: x 已在栈 → STORE tmp; LOAD_CONST 0; LOAD_VAR tmp; ISUB
            // tmp 用一个专用 slot（全局 0 如果有冲突风险，但这里只是编译期临时）
            // 为了安全，用一个全局变量 __neg_tmp
            // 但这会污染全局命名空间... 
            // 最终方案：直接在栈上操作 — 用一个简单的 NEG 指令
            // 暂时用：pop x, push -x 的方式在运行时处理
            // 编译器层面：生成 ITOD(如果needDouble) 然后调用一个运行时 neg
            // 最简方案：x; ITOD(如果需要); 然后用 0 - x
            // 由于我们没有 DUP，这里用一个临时局部/全局变量
            // 使用全局临时槽（用一个特殊名称，确保唯一）
            uint16_t tmpSlot = syms.allocGlobal("__neg_tmp");
            currentCode->emit(0x04); // STORE tmp  (保存 x)
            currentCode->emit(0x01); // LOAD_CONST 0
            currentCode->u16(cp.addInt(0));
            if (d || needDouble) {
                currentCode->emit(0x0E); // ITOD
            }
            currentCode->emit(0x05); // LOAD_VAR tmp
            currentCode->u16(tmpSlot);
            if (d || needDouble) {
                currentCode->emit(0x0E); // ITOD
                currentCode->emit(0x0B); // FSUB
            } else {
                currentCode->emit(0x07); // ISUB
            }
            return d || needDouble;
        }
        return primary(needDouble);
    }

    bool primary(bool needDouble) {
        if (cur.type == TK_NUM) {
            if (cur.isFloat) {
                currentCode->emit(0x01);
                currentCode->u16(cp.addDouble(cur.num));
                advance();
                return true;
            } else {
                currentCode->emit(0x01);
                currentCode->u16(cp.addInt((int32_t)cur.num));
                advance();
                return false;
            }
        } else if (cur.type == TK_STR) {
            currentCode->emit(0x01);
            currentCode->u16(cp.addString(cur.text));
            advance();
            return false;
        } else if (cur.type == TK_BOOL) {
            currentCode->emit(0x01);
            currentCode->u16(cp.addInt(cur.num ? 1 : 0));
            advance();
            return false; // bool 本质是 int (VAL_BOOL)
        } else if (cur.type == TK_LPAREN) {
            advance();
            bool d = compare(needDouble);
            expect(TK_RPAREN, ")");
            return d;
        } else if (cur.type == TK_IDENT) {
            string name = cur.text;
            Lexer peekL = L;
            Token peek = peekL.next();
            if (peek.type == TK_LPAREN) {
                callExpr();
                return false;
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
                return false;
            }
        } else {
            throw runtime_error("语法错误: 无法解析 '" + cur.text + "'");
        }
    }

    // 函数调用
    void callExpr(bool discardReturn = false) {
        string funcName = cur.text;
        advance(); // func name
        expect(TK_LPAREN, "(");

        if (funcName == "pr" || funcName == "prln" || funcName == "inp") {
            uint8_t argc = 0;
            if (cur.type != TK_RPAREN) {
                argc = 1;
                compare(false);
                while (cur.type == TK_COMMA) {
                    advance();
                    compare(false);
                    argc++;
                }
            }
            expect(TK_RPAREN, ")");
            uint8_t bid;
            if (funcName == "pr") bid = BID_pr;
            else if (funcName == "prln") bid = BID_prln;
            else bid = BID_inp;
            currentCode->emit(0x02);
            currentCode->u8(bid);
            currentCode->u8(argc);
        } else {
            auto it = syms.functions.find(funcName);
            if (it == syms.functions.end()) throw runtime_error("未定义的函数: " + funcName);
            uint16_t funcId = it->second;
            uint16_t expectedArgs = syms.funcParamCounts[funcId];

            uint8_t argc = 0;
            if (cur.type != TK_RPAREN) {
                argc = 1;
                compare(false);
                while (cur.type == TK_COMMA) {
                    advance();
                    compare(false);
                    argc++;
                }
            }
            expect(TK_RPAREN, ")");

            if (argc != (uint8_t)expectedArgs) {
                throw runtime_error("函数 '" + funcName + "' 参数数量不匹配: 期望 " +
                    to_string(expectedArgs) + "，得到 " + to_string(argc));
            }

            currentCode->emit(0x41); // CALL_FUNC
            currentCode->u16(funcId);
            currentCode->u8(argc);

            // 语句级调用（如单独一行的 foo()）：返回值无人接收，必须丢弃，
            // 否则会残留在操作数栈上，既浪费空间又会被后续的 pr() 误打印。
            if (discardReturn) currentCode->emit(0x43); // POP
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
        f.write((const char*)out.data(), out.size());
    }
};

// =============================================================================
//  main
// =============================================================================
int main(int argc, char** argv) {
    string inPath, outPath;
    if (argc > 1) {
        inPath = argv[1];
        if (argc > 2) outPath = argv[2];
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