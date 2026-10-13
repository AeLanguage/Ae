#pragma once
// 词法分析（Token / Lexer，含字符串插值展开）
// 纯前端：只依赖源码文本，不产生任何字节码，AOT 与字节码后端共用。
#include <string>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <algorithm>   // upper_bound（偏移→行号换算要用）
#include "ae_common.h"

// 词法分析（ 新增 TK_NULL / TK_SEMI）
enum TK {
    TK_EOF, TK_NUM, TK_STR, TK_IDENT, TK_BOOL, TK_NULL, TK_ASSIGN,
    TK_PLUS, TK_MINUS, TK_MUL, TK_DIV, TK_MOD,
    TK_EQ, TK_NEQ, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_ADDEQ, TK_SUBEQ, TK_MULEQ, TK_DIVEQ,
    TK_FUNC, TK_IF, TK_ELSE, TK_WHILE, TK_BREAK, TK_CONTINUE,
    TK_RETURN, TK_LOCAL,
    TK_LPAREN, TK_RPAREN, TK_COMMA, TK_LBRACE, TK_RBRACE, TK_LBRACKET, TK_RBRACKET,
    TK_SEMI,  // 必须追加在末尾：compare() 依赖 TK_EQ..TK_GE 的连续区间
    // P1-4 / P1-7：逻辑运算与三元（同样追加在末尾）
    TK_AND, TK_OR, TK_NOT, TK_QUESTION, TK_COLON,
    // P1-3：for / in
    TK_FOR, TK_IN,
    // P2-1 / P2-2 / P2-4：位运算、幂、do-while
    TK_AMP, TK_PIPE, TK_CARET, TK_TILDE, TK_SHL, TK_SHR, TK_POW, TK_DO,
    // 原生库：imp xxx / 限定名 xxx.func
    TK_IMP, TK_DOT, TK_ELLIPSIS,
    // 错误捕获
    TK_TRY, TK_CATCH,
    TK_DEFER, TK_FINALLY
};

struct Token {
    TK type; string text; double num; bool isFloat;
    int64_t ival = 0;      // P0-2：整数常量按 int64 保存（double 存不下全部 int64）
    int line = 1, col = 1; // P1-1：token 起始位置（1-based）
    Token() : type(TK_EOF), num(0), isFloat(false) {}
    Token(TK t, const string& tx = "", double n = 0, bool f = false)
        : type(t), text(tx), num(n), isFloat(f) {}
};

class Lexer {
    string src; size_t pos;
    // 字符串插值：双引号里的 "a${x}b" 在词法阶段就展开成等价的
    // ("a" + (x) + "b") 这串 token，next() 先从这里取。
    // 好处是解析器完全不用改 —— 它看到的就是一个普通的括号表达式。
    vector<Token> pending; size_t pendingPos = 0;
public:
    // P1-1：行首偏移表 —— 把 token 起始偏移换算成「行:列」
    vector<size_t> lineStarts;
    size_t tokStart = 0;
    uint16_t fileIdx = 0;            // 当前源文件在文件表里的下标（模块编译用）

    void buildLines() {
        lineStarts.clear();
        lineStarts.push_back(0);
        for (size_t i = 0; i < src.size(); i++)
            if (src[i] == '\n') lineStarts.push_back(i + 1);
    }
    void lineCol(size_t off, int& line, int& col) const {
        if (lineStarts.empty()) { line = 1; col = 1; return; }
        size_t i = (size_t)(std::upper_bound(lineStarts.begin(), lineStarts.end(), off) - lineStarts.begin());
        if (i == 0) i = 1;
        if (i > lineStarts.size()) i = lineStarts.size();
        line = (int)i;
        col  = (int)(off - lineStarts[i - 1] + 1);
    }
    // P1-1：词法阶段的诊断也带位置（未闭合字符串 / 非法字符 / 常量越界）
    string atLex(const string& msg, int len = 1) const {
        int l = 1, c = 1;
        lineCol(tokStart, l, c);
        return aePack(fileIdx, l, c, len, msg);
    }

    Lexer(const string& s) : src(s), pos(0) { buildLines(); }
    const string& source() const { return src; }
    Lexer() : src(), pos(0) {}
    void reset(const string& s) { src = s; pos = 0; tokStart = 0; buildLines(); pending.clear(); pendingPos = 0; }
    // P2-7：块注释 /* ... */（不嵌套；未闭合报错）
    void skip() {
        for (;;) {
            while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n' || src[pos] == '\r')) pos++;
            if (pos + 1 < src.size() && src[pos] == '/' && src[pos + 1] == '*') {
                size_t start = pos;
                pos += 2;
                while (pos + 1 < src.size() && !(src[pos] == '*' && src[pos + 1] == '/')) pos++;
                if (pos + 1 >= src.size()) { tokStart = start; throw runtime_error(atLex("块注释 /* 未闭合")); }
                pos += 2;
                continue;
            }
            break;
        }
    }
    // P1-1：next() 只负责记录 token 起点并补上行列，扫描逻辑在 scanBody()
    Token next() {
        // 插值展开出来的 token 优先吐出（它们已经带好正确的行列）
        if (pendingPos < pending.size()) return pending[pendingPos++];
        skip();
        tokStart = pos;
        if (pos >= src.size()) return {TK_EOF};
        Token t = scanBody();
        lineCol(tokStart, t.line, t.col);
        return t;
    }
    // 字符串插值：一个插值字符串切成若干片段
    // isExpr=false → 普通文本；isExpr=true → text 是 ${} 里的表达式源码，off 是它在 src 里的偏移
    struct StrPart { bool isExpr; string text; size_t off; };

    // 解析一个转义序列（pos 指向 '\\'）；\xHH / \uXXXX 可能产生多字节，所以返回 string
    string readEscapeSeq() {
        char e = src[pos + 1];
        pos += 2;
        switch (e) {
            case 'n':  return "\n";
            case 't':  return "\t";
            case 'r':  return "\r";
            case '0':  return string(1, '\0');
            case '\\': return "\\";
            case '"':  return "\"";
            case '\'': return "'";
            case 'a':  return "\a";
            case 'b':  return "\b";
            case 'f':  return "\f";
            case 'v':  return "\v";
            case 'x': {
                if (pos + 1 >= src.size() ||
                    !isxdigit((unsigned char)src[pos]) || !isxdigit((unsigned char)src[pos+1]))
                    throw runtime_error(atLex("\\x 转义需要两位十六进制数字"));
                string r(1, (char)stoi(src.substr(pos, 2), nullptr, 16));
                pos += 2;
                return r;
            }
            case 'u': {
                if (pos + 3 >= src.size())
                    throw runtime_error(atLex("\\u 转义需要四位十六进制数字"));
                for (int k = 0; k < 4; k++)
                    if (!isxdigit((unsigned char)src[pos + k]))
                        throw runtime_error(atLex("\\u 转义需要四位十六进制数字"));
                string r = utf8Encode((unsigned)stoi(src.substr(pos, 4), nullptr, 16));
                pos += 4;
                return r;
            }
            default:   return string(1, e);   // 未识别：保留字符本身
        }
    }

    // 扫完一个字符串的内容（pos 已在开引号之后），切成片段；返回是否含插值
    bool scanStrParts(char q, vector<StrPart>& parts) {
        const bool allowInterp = (q == '"');   // 单引号不插值
        string s;
        bool has = false;
        while (pos < src.size() && src[pos] != q) {
            if (src[pos] == '\\' && pos + 1 < src.size()) {
                // \$ 只在双引号里有特殊含义（输出字面的 $，关掉插值）
                if (allowInterp && src[pos + 1] == '$') { s += '$'; pos += 2; continue; }
                s += readEscapeSeq();
                continue;
            }
            if (allowInterp && src[pos] == '$' && pos + 1 < src.size() && src[pos+1] == '{') {
                if (!s.empty()) { parts.push_back({false, s, 0}); s.clear(); }
                size_t off = pos + 2;
                pos += 2;
                // 边扫边把"转义解码后"的表达式源码攒进 buf。
                // 必须解码：外层是双引号字符串，写 ${t[\"k\"]} 时 \" 是转义的引号，
                // 原样丢给子 Lexer 会被当成裸反斜杠报"非法字符"。
                // 只解引号和反斜杠，\n 之类保持原样留给子 Lexer 自己处理。
                string buf;
                int depth = 1;
                while (pos < src.size()) {
                    char ch = src[pos];
                    // 内嵌字符串：整体搬进 buf，里面的 { } 不参与深度计数
                    if (ch == '"' || ch == '\'') {
                        char iq = ch;
                        buf += ch; pos++;
                        while (pos < src.size() && src[pos] != iq) {
                            if (src[pos] == '\\' && pos + 1 < src.size()) {
                                char nx = src[pos + 1];
                                if (nx == '"' || nx == '\'' || nx == '\\') { buf += nx; pos += 2; }
                                else { buf += src[pos]; buf += nx; pos += 2; }
                                continue;
                            }
                            buf += src[pos++];
                        }
                        if (pos < src.size()) buf += src[pos++];     // 收尾引号
                        continue;
                    }
                    if (ch == '\\' && pos + 1 < src.size()) {
                        char nx = src[pos + 1];
                        if (nx == '"' || nx == '\'' || nx == '\\') { buf += nx; pos += 2; }
                        else { buf += ch; buf += nx; pos += 2; }
                        continue;
                    }
                    if (ch == '{') depth++;
                    else if (ch == '}') { depth--; if (depth == 0) { pos++; break; } }
                    buf += ch; pos++;
                }
                if (depth != 0) throw runtime_error(atLex("字符串插值 ${...} 未闭合（缺少 }）"));
                string expr = buf;
                if (expr.empty()) throw runtime_error(atLex("字符串插值 ${} 里是空的"));
                parts.push_back({true, expr, off});
                has = true;
                continue;
            }
            s += src[pos++];
        }
        if (pos >= src.size())
            throw runtime_error(atLex("字符串字面量未闭合（缺少收尾引号 " + string(1, q) + "）"));
        pos++;   // 收尾引号
        if (!s.empty() || parts.empty()) parts.push_back({false, s, 0});
        return has;
    }

    // 把 "a${x}b" 展开成 ("a" + (x) + "b") 的 token 流 —— 解析器看到的就是普通括号表达式
    // 表达式自带一层括号，所以 "v=${x}" * 2 这类不会被运算符优先级带偏。
    void buildInterpTokens(const vector<StrPart>& parts) {
        int line = 1, col = 1;
        lineCol(tokStart, line, col);
        auto mk = [&](TK t, const string& tx = "") {
            Token tk(t, tx); tk.line = line; tk.col = col; return tk;
        };
        size_t n = 0;
        for (const StrPart& p : parts) if (p.isExpr || !p.text.empty()) n++;

        pending.clear(); pendingPos = 0;
        if (n > 1) pending.push_back(mk(TK_LPAREN));
        bool first = true;
        for (const StrPart& p : parts) {
            if (!p.isExpr) {
                if (p.text.empty()) continue;
                if (!first) pending.push_back(mk(TK_PLUS));
                pending.push_back(mk(TK_STR, p.text));
                first = false;
                continue;
            }
            if (!first) pending.push_back(mk(TK_PLUS));
            pending.push_back(mk(TK_LPAREN));
            // 表达式交给一个子 Lexer 词法化，再把行列平移回它在源码里的真实位置
            Lexer sub(p.text);
            sub.fileIdx = fileIdx;
            int l0 = 1, c0 = 1;
            lineCol(p.off, l0, c0);
            Token t = sub.next();
            while (t.type != TK_EOF) {
                int sl = t.line, sc = t.col;
                t.line = l0 + (sl - 1);
                t.col  = (sl == 1) ? c0 + (sc - 1) : sc;
                pending.push_back(t);
                t = sub.next();
            }
            pending.push_back(mk(TK_RPAREN));
            first = false;
        }
        if (n > 1) pending.push_back(mk(TK_RPAREN));
    }

    Token scanBody() {
        char c = src[pos];

        if (c == '/' && pos + 1 < src.size() && src[pos+1] == '/') {
            while (pos < src.size() && src[pos] != '\n') pos++;
            return next();
        }
        if (c == '"' || c == '\'') {
            char q = c; pos++;
            // 字符串插值：双引号里的 ${...} 会被切出来；单引号是"原始字符串"，不插值
            // （想输出字面的 ${x} 就用单引号，省得写 \$ 转义）
            vector<StrPart> parts;
            bool interp = scanStrParts(q, parts);
            if (!interp) {
                string s;
                for (const StrPart& p : parts) s += p.text;
                return {TK_STR, s, 0, false};
            }
            buildInterpTokens(parts);
            return pending[pendingPos++];
        }
        if (isdigit(c) || (c == '.' && pos+1 < src.size() && isdigit(src[pos+1]))) {
            // P2-3：0x / 0b 前缀、_ 分隔符、科学计数法
            string num;
            bool isFloat = false;
            int  base = 10;
            if (c == '0' && pos + 1 < src.size() && (src[pos+1] == 'x' || src[pos+1] == 'X')) {
                base = 16; pos += 2;
                while (pos < src.size() && (isxdigit((unsigned char)src[pos]) || src[pos] == '_')) {
                    if (src[pos] != '_') num += src[pos];
                    pos++;
                }
                if (num.empty()) throw runtime_error(atLex("十六进制字面量缺少数字"));
            } else if (c == '0' && pos + 1 < src.size() && (src[pos+1] == 'b' || src[pos+1] == 'B')) {
                base = 2; pos += 2;
                while (pos < src.size() && (src[pos] == '0' || src[pos] == '1' || src[pos] == '_')) {
                    if (src[pos] != '_') num += src[pos];
                    pos++;
                }
                if (num.empty()) throw runtime_error(atLex("二进制字面量缺少数字"));
            } else {
                if (c == '.') num += '0';
                while (pos < src.size() && (isdigit((unsigned char)src[pos]) || src[pos] == '.' || src[pos] == '_')) {
                    if (src[pos] == '.') { if (isFloat) break; isFloat = true; }
                    if (src[pos] != '_') num += src[pos];
                    pos++;
                }
                if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
                    size_t save = pos;
                    string ex = "e";
                    pos++;
                    if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) { ex += src[pos]; pos++; }
                    if (pos < src.size() && isdigit((unsigned char)src[pos])) {
                        while (pos < src.size() && (isdigit((unsigned char)src[pos]) || src[pos] == '_')) {
                            if (src[pos] != '_') ex += src[pos];
                            pos++;
                        }
                        num += ex; isFloat = true;
                    } else pos = save;      // 不是指数（例如标识符 e1）→ 回退
                }
            }
            Token t; t.type = TK_NUM; t.text = num; t.isFloat = isFloat;
            if (base != 10) {
                // 十六/二进制按位模式解析（0xFFFFFFFFFFFFFFFF → -1）
                try {
                    uint64_t u = stoull(num, nullptr, base);
                    memcpy(&t.ival, &u, 8);
                } catch (...) { throw runtime_error(atLex("整数常量超出 64 位范围: " + num)); }
                t.num = (double)t.ival;
            } else if (isFloat) {
                try { t.num = stod(num); }
                catch (...) { throw runtime_error(atLex("浮点常量超出 double 范围: " + num)); }
            } else {
                try { t.ival = (int64_t)stoll(num); }
                catch (...) { throw runtime_error(atLex("整数常量超出 int64 范围: " + num)); }
                t.num = (double)t.ival;
            }
            return t;
        }
        if (isalpha(c) || c == '_') {
            string id; while (pos < src.size() && (isalnum(src[pos]) || src[pos] == '_')) id += src[pos++];
            if (id == "if")       return {TK_IF, id};
            if (id == "else")     return {TK_ELSE, id};
            if (id == "while")    return {TK_WHILE, id};
            if (id == "do")       return {TK_DO, id};       // P2-4
            if (id == "imp")      return {TK_IMP, id};      // 导入原生库
            if (id == "try")      return {TK_TRY, id};      // 错误捕获
            if (id == "catch")    return {TK_CATCH, id};
            if (id == "defer")    return {TK_DEFER, id};      // 延迟执行
            if (id == "finally")  return {TK_FINALLY, id};    // try/catch/finally
            if (id == "for")      return {TK_FOR, id};      // P1-3
            if (id == "in")       return {TK_IN, id};       // P1-3
            if (id == "break")    return {TK_BREAK, id};
            if (id == "continue") return {TK_CONTINUE, id};
            if (id == "return")   return {TK_RETURN, id};
            if (id == "local")    return {TK_LOCAL, id};
            if (id == "func")     return {TK_FUNC, id};
            // P1-4：逻辑运算关键字
            if (id == "and")      return {TK_AND, id};
            if (id == "or")       return {TK_OR, id};
            if (id == "not")      return {TK_NOT, id};
            if (id == "true")     return {TK_BOOL, id, 1, false};
            if (id == "false")    return {TK_BOOL, id, 0, false};
            if (id == "null")     return {TK_NULL, id};
            if (id == "type")     return {TK_IDENT, id};             // 内置
            if (id == "len")      return {TK_IDENT, id};
            if (id == "str")      return {TK_IDENT, id};
            if (id == "int")      return {TK_IDENT, id};
            if (id == "float")    return {TK_IDENT, id};
            if (id == "pr")       return {TK_IDENT, id};
            if (id == "prln")     return {TK_IDENT, id};
            if (id == "inp")      return {TK_IDENT, id};
            if (id == "main")     return {TK_IDENT, id};
            return {TK_IDENT, id, 0, false};
        }
        pos++;
        // P2-1 / P2-2：位运算与幂（必须在单字符运算符之前判断）
        if (c == '<' && pos < src.size() && src[pos] == '<') { pos++; return {TK_SHL, "<<", 0, false}; }
        if (c == '>' && pos < src.size() && src[pos] == '>') { pos++; return {TK_SHR, ">>", 0, false}; }
        if (c == '*' && pos < src.size() && src[pos] == '*') { pos++; return {TK_POW, "**", 0, false}; }
        if (c == '&') { return {TK_AMP, "&", 0, false}; }
        if (c == '|') { return {TK_PIPE, "|", 0, false}; }
        if (c == '^') { return {TK_CARET, "^", 0, false}; }
        if (c == '~') { return {TK_TILDE, "~", 0, false}; }
        // 复合赋值必须先于单字符运算符判断（此前词法器从不产出这些 token）
        if (c == '+' && pos < src.size() && src[pos] == '=') { pos++; return {TK_ADDEQ, "+=", 0, false}; }
        if (c == '-' && pos < src.size() && src[pos] == '=') { pos++; return {TK_SUBEQ, "-=", 0, false}; }
        if (c == '*' && pos < src.size() && src[pos] == '=') { pos++; return {TK_MULEQ, "*=", 0, false}; }
        if (c == '/' && pos < src.size() && src[pos] == '=') { pos++; return {TK_DIVEQ, "/=", 0, false}; }
        if (c == '=' && pos < src.size() && src[pos] == '=') { pos++; return {TK_EQ, "==", 0, false}; }
        if (c == '!' && pos < src.size() && src[pos] == '=') { pos++; return {TK_NEQ, "!=", 0, false}; }
        if (c == '<' && pos < src.size() && src[pos] == '=') { pos++; return {TK_LE, "<=", 0, false}; }
        if (c == '>' && pos < src.size() && src[pos] == '=') { pos++; return {TK_GE, ">=", 0, false}; }
        if (c == '+') { return {TK_PLUS, "+", 0, false}; }
        if (c == '-') { return {TK_MINUS, "-", 0, false}; }
        if (c == '*') { return {TK_MUL, "*", 0, false}; }
        if (c == '/') { return {TK_DIV, "/", 0, false}; }
        if (c == '%') { return {TK_MOD, "%", 0, false}; }
        switch (c) {
            case '=': return {TK_ASSIGN, "=", 0, false};
            case '<': return {TK_LT, "<", 0, false};
            case '>': return {TK_GT, ">", 0, false};
            case '(': return {TK_LPAREN, "(", 0, false};
            case ')': return {TK_RPAREN, ")", 0, false};
            case ',': return {TK_COMMA, ",", 0, false};
            case '{': return {TK_LBRACE, "{", 0, false};
            case '}': return {TK_RBRACE, "}", 0, false};
            case '[': return {TK_LBRACKET, "[", 0, false};
            case ']': return {TK_RBRACKET, "]", 0, false};
            case ';': return {TK_SEMI, ";", 0, false};   // 空语句 / 语句分隔符
            case '.':
                // 变参：只有紧跟两个点才是 ...（否则 `a.b` 里的点照旧是 TK_DOT）
                if (pos + 1 < src.size() && src[pos] == '.' && src[pos + 1] == '.') {
                    pos += 2; return {TK_ELLIPSIS, "...", 0, false};
                }
                return {TK_DOT, ".", 0, false};    // 限定库函数调用 io.readFile
            case '?': return {TK_QUESTION, "?", 0, false};   // P1-7
            case ':': return {TK_COLON, ":", 0, false};      // P1-7
            // 非法字符直接报错：此前返回 TK_EOF 会让剩余全部代码被静默丢弃
            // 修：多字节字符要整字打出来（以前只印首字节，终端里显示成乱码 '�'），
            // 中文标识符又是新手最常踩的坑，顺手给一句提示
            default: {
                size_t i = tokStart, n = 1;
                unsigned char b0 = (unsigned char)src[i];
                if (b0 >= 0xF0) n = 4; else if (b0 >= 0xE0) n = 3; else if (b0 >= 0xC0) n = 2;
                if (i + n > src.size()) n = 1;
                std::string ch = src.substr(i, n);
                std::string msg = "非法字符: '" + ch + "'";
                if (b0 >= 0x80 && !(c == '"' || c == '\''))
                    msg += "（标识符只能用 ASCII 字母、数字、下划线；中文写在字符串里没问题）";
                throw runtime_error(atLex(msg, (int)n));
            }
        }
    }
};
