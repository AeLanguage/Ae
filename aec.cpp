// =============================================================================
//  Ae 编译器 (aec = Ae Compiler)  ——  支持自定义函数 + 局部变量 + 递归
//                                      + 字符串拼接 + len/str/int/float/type 内置
//                                      + 【数组】{e1, e2, ...} / a[i] / a[i] = v
// -----------------------------------------------------------------------------
//  ▌本版新增（值的基石）：
//      · null 字面量：a = null        （TK_NULL，编译为 LOAD_CONST 常量池 NULL 项）
//      · type(x) 内置函数 → 返回类型名字符串（"null"/"int"/"float"/"string"/"bool"/"array"）
//      · 严格类型判等：类型不同即 false（1 != 1.0、null != false）
//      · ValueKind 编号迁移：NULL=0 打头，ARR=5 在最后（与 VM 完全一致）
//
//  ▌★ 本轮修复（P0 批次：语义错误与未定义行为）：
//      · P0-1 块作用域：SymbolTable 改为作用域栈，每个 { } / if / while 体
//        独立作用域。此前 allocLocal 对同名变量永远复用同一槽位，
//        `local x = 1; if (true) { local x = 99 }` 会把外层 x 改成 99，
//        且块内 local 在块外仍可见。
//      · P0-2 整数常量按 int64 入池（小数仍用 INT32 tag）：此前用 stoi，
//        `prln(3000000000)` 直接抛出 C++ 原始异常 "stoi argument out of range"。
//      · 删除遗留的 [DBG] stderr 输出；表达式语句补 POP（操作数栈不再泄漏）；
//        下标赋值的临时槽不再用 0xFFFF（此前会让 globals 扩到 65536 项）
//
//  ▌★ P1 批次（可用性）：
//      · P1-1 行列号诊断：所有诊断带「第 N 行 M 列」；字节码新增 源文件名 /
//        函数名表 / 行号表 三段（minor=1），VM 出错时打印调用栈与所在行。
//      · P1-2 字节码校验：major/minor 版本校验、常量池下标、函数体范围检查。
//      · P1-8/P1-9 顶层语句编译进隐式脚本函数 __script —— 于是顶层能用 local、
//        能用 return；顶层执行完自动调用 main（main 必须无参）。
//      · P1-10 调用约定：RET 带返回值个数，VM 用帧的 stackBase 清掉函数内残留。
//      · P1-5 多返回值/多赋值：`return a, b`、`local a, b = f()`、`a, b = b, a`；
//        调用指令带 want（0=丢弃 / 1=取第一个 / k=恰好 k 个）。
//        实参位置的多值展开与 `...` 变参留到下一批。
//      · P1-4 and/or/not：短路且返回操作数（Lua 风格）；not 返回真 bool。
//      · P1-7 三元 `cond ? a : b`，惰性求值。
//      · P1-3 for：数值 `for i = a, b [, step]`（含端点，step 为 0 报错）与
//        遍历 `for k, v in t` / `for v in t`（数组段 + 哈希段，顺序确定，快照语义）。
//      · P1-11 读取从未被赋值的名字 → 编译错误（拼写错误立刻暴露）。
//      · P1-12 源码强制 UTF-8（含 BOM 跳过），len(字符串) 按【字符】计数。
//      · P1-6 错误设施：assert / error / exit；退出码 0 成功 / 1 编译错误 /
//        2 运行时错误 / 3 用法与 IO 错误。
//
//  ▌★ P2 批次（表达力与细节；工程类只做了 Test/ 回归套件）：
//      · P2-1/2 位运算 `& | ^ ~ << >>`（只接受 int；移位量按 0..63 取模，
//        算术右移手工实现）；幂 `**`（右结合，整数底+非负整数指数走整数回绕，
//        否则用浮点 pow）。位运算优先级【高于】比较，避免 C 的 `x & 1 == 0` 坑。
//      · P2-3 数字字面量：`0x` / `0b` / `_` 分隔符 / 科学计数法。
//      · P2-4 `do { } while (cond)`；P2-5 `break N` / `continue N`（N 为循环层数）。
//      · P2-6 转义补全 `\xHH` `\uXXXX` `\a\b\f\v`；P2-7 块注释 `/* */`。
//      · P2-9 浮点打印统一为【最短往返表示】，`prln` 与 `str` 一致
//        （2.0 → "2.0"、1/3 → "0.3333333333333333"）。
//      · P2-10 整数除法/取模向下取整（`-7 / 3 == -3`、`-7 % 3 == 2`）。
//      · P2-11 字符串 `s[i]`（1-based 按码点，越界 null）+ 内建
//        substr/ord/chr/find/upper/lower/trim/replace/split/join/repeat。
//      · P2-12 `format(fmt, ...)` printf 风格（%d %s %f %x %o %c %% + 宽度/精度）。
//      · P2-13 `len()` 对 float/bool/null 报错（不再静默 0）。
//      · P2-14 表原语 delete/insert/pop/append/count。
//      · P2-15 稀疏整数键（> 数组长度+8 的整数键走哈希段）。
//      · P2-16 copy（浅）/ deepcopy（深，环安全）/ equal（深相等，环安全）。
//      · P2-17 表键不能是 float（报错，要求显式 int(x)/str(x)）。
//      · P2-18 函数重复定义报错；P2-19 内建名不能被用户函数遮蔽。
//      · P2-28 `inp(提示)` 会真正输出提示，EOF 返回 null 而不是抛裸错误。
//
//  ▌★ 语义规范（与 ae.cpp 的 doCompare/doArith 严格一致）：
//      判等 == / !=
//        · 与 null 比较永远合法：只有 null == null 为 true
//        · int 与 float 之间【运行时报错】——同一数值域的不同表示，
//          静默判为不等最易踩坑（应写 1.0 或 float(x)）
//        · 其余类型不同 → false / true（不同类型本来就不相等）
//      大小比较 < <= > >=
//        · 类型不同 → 运行时报错；同类型里 null / 表 → 报错
//      算术 + - * / %
//        · int 与 float 自动提升；字符串只有 + 可拼接；
//        · bool 不是数值，true + 1 报错（显式 int(x) 可用）；
//        · null / 表参与算术报错；整数溢出为确定的二进制回绕
//      转换 int() / float()
//        · 解析失败或类型不支持 → 运行时报错（不再静默给 0）
//
//  ▌字节码约定（须与 ae.cpp 一致）：
//      0x50  NEW_TABLE u16(count)       构造表（数组段 + 哈希段）
//      0x51  TABLE_GET                  [table, key] → value
//      0x52  TABLE_SET                  [table, key, val] → 就地修改
//      0x1C  LOAD_TRUE                  [ ] → true
//      0x1D  LOAD_FALSE                 [ ] → false
// =============================================================================

#ifdef _WIN32
  #define _CRT_SECURE_NO_WARNINGS
#endif

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
#include <algorithm>
#include <cctype>
#include "ae_libhost.h"      // ★ 原生库加载（aec 只用它读导出表做编译期检查）
#include "ae_diag.h"         // ★ 诊断系统（错误码 + 源码摘录 + 波浪线）
#include "ae_console.h"     // ★ 控制台中文：绕开代码页

// ★ 结构化诊断消息：\x01 行 \x01 列 \x01 长度 \x01 消息
//   用它而不是改 20 多处 throw —— 错误分类集中在 renderDiag() 一处维护
static std::string aePack(int file, int line, int col, int len, const std::string& msg) {
    return std::string("\x01") + std::to_string(file) + "\x01" + std::to_string(line) + "\x01" +
           std::to_string(col) + "\x01" + std::to_string(len) + "\x01" + msg;
}
static bool aeUnpack(const std::string& what, int& file, int& line, int& col, int& len, std::string& msg) {
    if (what.empty() || what[0] != '\x01') return false;
    size_t p = 1;
    int* out[4] = { &file, &line, &col, &len };
    for (int i = 0; i < 4; i++) {
        size_t q = what.find('\x01', p);
        if (q == std::string::npos) return false;
        *out[i] = atoi(what.substr(p, q - p).c_str());
        p = q + 1;
    }
    msg = what.substr(p);
    return true;
}

using namespace std;

// ★ P2-6：把 Unicode 码点编码成 UTF-8（供 \uXXXX 转义使用）
static string utf8Encode(unsigned cp) {
    string s;
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) {
        s += (char)(0xC0 | (cp >> 6));
        s += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += (char)(0xE0 | (cp >> 12));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    } else {
        s += (char)(0xF0 | (cp >> 18));
        s += (char)(0x80 | ((cp >> 12) & 0x3F));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    }
    return s;
}

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
//  常量池（★ 新增 NULL 类型 tag）
// =============================================================================
struct Const {
    enum { UTF8 = 1, INT32 = 2, DOUBLE = 3, NULLV = 4, INT64 = 5 } tag;   // ★ NULLV / INT64
    string  s;
    int32_t i;
    int64_t i64 = 0;    // ★ P0-2
    double  d;
};

class ConstantPool {
public:
    vector<Const> pool;

    uint16_t addNull() {
        for (uint16_t k = 0; k < (uint16_t)pool.size(); k++)
            if (pool[k].tag == Const::NULLV) return k;
        Const c; c.tag = Const::NULLV; pool.push_back(c); return (uint16_t)pool.size() - 1;
    }
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
    // ★ P0-2：运行期是 int64，常量池必须能装下整个 int64
    uint16_t addInt64(int64_t v) {
        for (uint16_t k = 0; k < (uint16_t)pool.size(); k++)
            if (pool[k].tag == Const::INT64 && pool[k].i64 == v) return k;
        Const c; c.tag = Const::INT64; c.i64 = v; pool.push_back(c); return (uint16_t)pool.size() - 1;
    }
    // 小整数仍用 INT32（字节码更紧凑，也与既有 .aeo 保持兼容）
    uint16_t addIntAuto(int64_t v) {
        if (v >= INT32_MIN && v <= INT32_MAX) return addInt((int32_t)v);
        return addInt64(v);
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
            } else if (c.tag == Const::INT64) {
                uint64_t bits = (uint64_t)c.i64;
                for (int s = 56; s >= 0; s -= 8) out.push_back((uint8_t)((bits >> s) & 0xFF));
            } else if (c.tag == Const::DOUBLE) {
                uint64_t bits; memcpy(&bits, &c.d, 8);
                for (int s = 56; s >= 0; s -= 8) out.push_back((uint8_t)((bits >> s) & 0xFF));
            }
            // NULLV：仅 tag 字节，无附加数据
        }
    }
};

// =============================================================================
//  字节码缓冲区
// =============================================================================
struct CodeBuf {
    vector<uint8_t> code;
    // ★ 指令偏移 → (源码行, 列)：运行期错误据此定位到具体位置
    struct LineEntry { uint32_t off, line, col; uint16_t file; };   // ★ file：模块编译后不止一个源文件
    vector<LineEntry> lines;
    uint32_t pendingLine = 1, pendingCol = 1;
    uint16_t pendingFile = 0;                    // advance() 更新，emit() 记录
    void markPos(uint32_t line, uint32_t col) {
        if (!lines.empty() && lines.back().line == line && lines.back().col == col &&
            lines.back().file == pendingFile) return;
        lines.push_back({(uint32_t)code.size(), line, col, pendingFile});
    }
    void markLine(uint32_t line) { markPos(line, 1); }
    void emit(uint8_t b) {
        markPos(pendingLine, pendingCol);
        code.push_back(b);
    }
    void u8(uint8_t v)  { code.push_back(v); }
    void u16(uint16_t v){ writeU16(code, v); }
    void u32(uint32_t v){ writeU32(code, v); }
    size_t size() const { return code.size(); }
    void patchU16At(size_t pos, uint16_t val) {
        code[pos]   = static_cast<uint8_t>((val >> 8) & 0xFF);
        code[pos+1] = static_cast<uint8_t>(val & 0xFF);
    }
    void patchU8At(size_t pos, uint8_t val) { code[pos] = val; }   // ★ P1-5
    void write(vector<uint8_t>& out) const {
        writeU32(out, (uint32_t)code.size());
        out.insert(out.end(), code.begin(), code.end());
    }
};

// =============================================================================
//  符号表
// =============================================================================
// ★ 1.7 闭包：上值描述（必须与 ae.cpp 里的 UpvalDesc 一致）
//    fromParentLocal=true  → 抓父帧的 locals[index]（引用捕获，共享变量）
//    fromParentLocal=false → 继承父闭包的第 index 个上值（多层嵌套的中转）
struct UpvalDesc {
    bool     fromParentLocal;
    uint16_t index;
};

struct VarInfo {
    enum Kind { GLOBAL, LOCAL, UPVAL } kind;   // ★ UPVAL = 闭包捕获的外部变量
    uint16_t slot;
};

class SymbolTable {
public:
    map<string, uint16_t> globals;
    uint16_t globalCount = 0;

    // =========================================================================
    //  ★ 词法作用域栈（P0-1 修复）
    //  旧实现用一张扁平的 name→slot 表：allocLocal 对同名变量永远返回同一个槽位，
    //  于是 `local x = 1; if (true) { local x = 99 }` 里的内层 local 直接改写了
    //  外层变量（实测 prln(x) 得 99），块外还能读到块内声明的变量。
    //
    //  现在：scopes[0] 是函数作用域，每个受控块进出时 push/pop；
    //    · 同名变量在不同作用域分到【不同槽位】→ 真正的遮蔽
    //    · 出作用域时水位回退 → 槽位可复用，帧大小只取决于嵌套深度
    //    · 作用域外不可见 → local 不再泄漏
    // =========================================================================
    struct Scope {
        map<string, uint16_t> names;
        uint16_t startSlot;      // 进入本作用域时的水位，退出时回退到这里
    };
    vector<Scope> scopes;

    uint16_t localCount = 0;        // 当前水位
    uint16_t maxLocalCount = 0;     // 高水位 = 函数帧需要的局部槽数

    map<string, uint16_t> functions;
    map<uint16_t, uint16_t> funcParamCounts;
    set<uint16_t> userFuncIds;

    bool inFunction = false;

    // =========================================================================
    //  ★ 1.7 闭包：当前函数的上值表 + 外层函数上下文栈
    //  编译嵌套函数时，把【当前】上下文搬进 ctxStack，换成新函数的；退出时恢复。
    //  访问外层变量时，resolveAux 递归向外找，并在【回程】逐层登记上值 —— 这正是
    //  Lua 的 singlevaraux 做法：最近的那层登记"抓父帧局部"，更外层登记"继承父上值"。
    // =========================================================================
    struct FuncCtx {
        vector<Scope> scopes;
        uint16_t localCount = 0;
        uint16_t maxLocalCount = 0;
        vector<UpvalDesc> upvals;
        vector<string>    upvalNames;
        string name;
    };
    vector<FuncCtx>   ctxStack;        // [0] = 最外层函数
    vector<UpvalDesc> curUpvals;       // 当前函数的上值表
    vector<string>    curUpvalNames;
    string curFuncName = "<script>";
    size_t curFuncId   = 0;

    void pushCtx(const string& name) {
        FuncCtx c;
        c.scopes = std::move(scopes);
        c.localCount = localCount;
        c.maxLocalCount = maxLocalCount;
        c.upvals = std::move(curUpvals);
        c.upvalNames = std::move(curUpvalNames);
        c.name = curFuncName;
        ctxStack.push_back(std::move(c));
        scopes.clear();
        localCount = 0;
        maxLocalCount = 0;
        curUpvals.clear();
        curUpvalNames.clear();
        curFuncName = name;
        inFunction = true;
        pushScope();
    }
    // 退出嵌套函数：交回它的上值表，并恢复外层上下文
    void popCtx(vector<UpvalDesc>* ups) {
        if (ctxStack.empty()) return;
        if (ups) *ups = std::move(curUpvals);
        FuncCtx c = std::move(ctxStack.back());
        ctxStack.pop_back();
        scopes = std::move(c.scopes);
        localCount = c.localCount;
        maxLocalCount = c.maxLocalCount;
        curUpvals = std::move(c.upvals);
        curUpvalNames = std::move(c.upvalNames);
        curFuncName = c.name;
    }

    enum ResKind { RES_NONE, RES_LOCAL, RES_UPVAL };
    uint16_t addUpvalue(size_t level, bool fromLocal, uint16_t index, const string& name) {
        size_t n = ctxStack.size();
        vector<UpvalDesc>* u  = (level == 0) ? &curUpvals : &ctxStack[n - level].upvals;
        vector<string>*    nm = (level == 0) ? &curUpvalNames : &ctxStack[n - level].upvalNames;
        for (size_t i = 0; i < u->size(); i++)
            if ((*u)[i].fromParentLocal == fromLocal && (*u)[i].index == index) return (uint16_t)i;
        u->push_back({fromLocal, index});
        nm->push_back(name);
        return (uint16_t)(u->size() - 1);
    }
    // level: 0 = 当前函数，1 = 直接外层 …
    ResKind resolveAux(size_t level, const string& name, uint16_t* slot) {
        size_t n = ctxStack.size();
        if (level > n) return RES_NONE;
        vector<Scope>* sc = (level == 0) ? &scopes : &ctxStack[n - level].scopes;
        for (size_t i = sc->size(); i-- > 0; ) {
            auto it = (*sc)[i].names.find(name);
            if (it != (*sc)[i].names.end()) { *slot = it->second; return RES_LOCAL; }
        }
        if (level >= n) return RES_NONE;
        ResKind outer = resolveAux(level + 1, name, slot);
        if (outer == RES_NONE) return RES_NONE;
        *slot = addUpvalue(level, outer == RES_LOCAL, *slot, name);
        return RES_UPVAL;
    }

    void beginFunction() {
        scopes.clear();
        localCount = 0;
        maxLocalCount = 0;
        curUpvals.clear();           // ★ 顶层函数没有上值
        curUpvalNames.clear();
        pushScope();                 // 函数作用域（参数与函数体顶层局部都在这）
    }
    void endFunction() {
        scopes.clear();
        localCount = 0;
    }
    void pushScope() {
        Scope s; s.startSlot = localCount;
        scopes.push_back(s);
    }
    void popScope() {
        if (scopes.empty()) return;
        localCount = scopes.back().startSlot;   // 回收本作用域槽位
        scopes.pop_back();
    }

    VarInfo resolve(const string& name) {
        if (inFunction) {
            // 由内向外逐层查找，先命中的是最内层同名绑定
            for (size_t i = scopes.size(); i-- > 0; ) {
                auto it = scopes[i].names.find(name);
                if (it != scopes[i].names.end()) return {VarInfo::LOCAL, it->second};
            }
        }
        // ★ 1.7：再看外层函数（闭包捕获）——会沿途登记上值，所以 resolve 不再是 const
        if (!ctxStack.empty()) {
            uint16_t us = 0;
            if (resolveAux(0, name, &us) == RES_UPVAL) return {VarInfo::UPVAL, us};
        }
        auto it = globals.find(name);
        if (it != globals.end()) return {VarInfo::GLOBAL, it->second};
        return {VarInfo::GLOBAL, (uint16_t)-1};
    }

    bool forbidNewGlobals = false;      // ★ 编译 .m 模块时置位：模块不许有隐式全局
    uint16_t allocGlobal(const string& name) {
        auto it = globals.find(name);
        if (it != globals.end()) return it->second;
        if (forbidNewGlobals)
            throw runtime_error("模块内不允许隐式全局变量（请写成 local " + name + " = ...）");
        uint16_t slot = globalCount++;
        globals[name] = slot;
        return slot;
    }
    // ★ 只在【当前作用域】内查重：嵌套块里的同名 local 会拿到新槽位（遮蔽）
    uint16_t allocLocal(const string& name) {
        if (scopes.empty()) throw runtime_error("内部错误：没有活动作用域");
        Scope& s = scopes.back();
        auto it = s.names.find(name);
        if (it != s.names.end()) return it->second;   // 同作用域内重复声明 → 同一槽位（同 Lua）
        uint16_t slot = bumpLocal();
        s.names[name] = slot;
        return slot;
    }
    uint16_t allocParam(const string& name) { return allocLocal(name); }

    // 表达式求值用的临时槽：不注册名字，随作用域回收
    uint16_t allocScratch() { return bumpLocal(); }

private:
    uint16_t bumpLocal() {
        uint16_t slot = localCount++;
        if (localCount > maxLocalCount) maxLocalCount = localCount;
        return slot;
    }
};

// =============================================================================
//  词法分析（★ 新增 TK_NULL / TK_SEMI）
// =============================================================================
enum TK {
    TK_EOF, TK_NUM, TK_STR, TK_IDENT, TK_BOOL, TK_NULL, TK_ASSIGN,
    TK_PLUS, TK_MINUS, TK_MUL, TK_DIV, TK_MOD,
    TK_EQ, TK_NEQ, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_ADDEQ, TK_SUBEQ, TK_MULEQ, TK_DIVEQ,
    TK_FUNC, TK_IF, TK_ELSE, TK_WHILE, TK_BREAK, TK_CONTINUE,
    TK_RETURN, TK_LOCAL,
    TK_LPAREN, TK_RPAREN, TK_COMMA, TK_LBRACE, TK_RBRACE, TK_LBRACKET, TK_RBRACKET,
    TK_SEMI,  // ★ 必须追加在末尾：compare() 依赖 TK_EQ..TK_GE 的连续区间
    // ★ P1-4 / P1-7：逻辑运算与三元（同样追加在末尾）
    TK_AND, TK_OR, TK_NOT, TK_QUESTION, TK_COLON,
    // ★ P1-3：for / in
    TK_FOR, TK_IN,
    // ★ P2-1 / P2-2 / P2-4：位运算、幂、do-while
    TK_AMP, TK_PIPE, TK_CARET, TK_TILDE, TK_SHL, TK_SHR, TK_POW, TK_DO,
    // ★ 原生库：imp xxx / 限定名 xxx.func
    TK_IMP, TK_DOT,
    // ★ 错误捕获
    TK_TRY, TK_CATCH
};

struct Token {
    TK type; string text; double num; bool isFloat;
    int64_t ival = 0;      // ★ P0-2：整数常量按 int64 保存（double 存不下全部 int64）
    int line = 1, col = 1; // ★ P1-1：token 起始位置（1-based）
    Token() : type(TK_EOF), num(0), isFloat(false) {}
    Token(TK t, const string& tx = "", double n = 0, bool f = false)
        : type(t), text(tx), num(n), isFloat(f) {}
};

class Lexer {
    string src; size_t pos;
public:
    // ★ P1-1：行首偏移表 —— 把 token 起始偏移换算成「行:列」
    vector<size_t> lineStarts;
    size_t tokStart = 0;
    uint16_t fileIdx = 0;            // ★ 当前源文件在文件表里的下标（模块编译用）

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
    // ★ P1-1：词法阶段的诊断也带位置（未闭合字符串 / 非法字符 / 常量越界）
    string atLex(const string& msg, int len = 1) const {
        int l = 1, c = 1;
        lineCol(tokStart, l, c);
        return aePack(fileIdx, l, c, len, msg);
    }

    Lexer(const string& s) : src(s), pos(0) { buildLines(); }
    const string& source() const { return src; }
    Lexer() : src(), pos(0) {}
    void reset(const string& s) { src = s; pos = 0; tokStart = 0; buildLines(); }
    // ★ P2-7：块注释 /* ... */（不嵌套；未闭合报错）
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
    // ★ P1-1：next() 只负责记录 token 起点并补上行列，扫描逻辑在 scanBody()
    Token next() {
        skip();
        tokStart = pos;
        if (pos >= src.size()) return {TK_EOF};
        Token t = scanBody();
        lineCol(tokStart, t.line, t.col);
        return t;
    }
    Token scanBody() {
        char c = src[pos];

        if (c == '/' && pos + 1 < src.size() && src[pos+1] == '/') {
            while (pos < src.size() && src[pos] != '\n') pos++;
            return next();
        }
        if (c == '"' || c == '\'') {
            char q = c; pos++;
            string s;
            while (pos < src.size() && src[pos] != q) {
                if (src[pos] == '\\' && pos + 1 < src.size()) {
                    // ★ 真正解析转义序列（此前只是把反斜杠丢掉，"\t" 得到 't'）
                    char e = src[pos + 1];
                    pos += 2;
                    switch (e) {
                        case 'n':  s += '\n'; break;
                        case 't':  s += '\t'; break;
                        case 'r':  s += '\r'; break;
                        case '0':  s += '\0'; break;
                        case '\\': s += '\\'; break;
                        case '"':  s += '"';  break;
                        case '\'': s += '\''; break;
                        case 'a':  s += '\a'; break;
                        case 'b':  s += '\b'; break;
                        case 'f':  s += '\f'; break;
                        case 'v':  s += '\v'; break;
                        case 'x': {   // ★ P2-6：\xHH
                            if (pos + 1 >= src.size() ||
                                !isxdigit((unsigned char)src[pos]) || !isxdigit((unsigned char)src[pos+1]))
                                throw runtime_error(atLex("\\x 转义需要两位十六进制数字"));
                            s += (char)stoi(src.substr(pos, 2), nullptr, 16);
                            pos += 2;
                            break;
                        }
                        case 'u': {   // ★ P2-6：\uXXXX → UTF-8
                            if (pos + 3 >= src.size())
                                throw runtime_error(atLex("\\u 转义需要四位十六进制数字"));
                            for (int k = 0; k < 4; k++)
                                if (!isxdigit((unsigned char)src[pos + k]))
                                    throw runtime_error(atLex("\\u 转义需要四位十六进制数字"));
                            s += utf8Encode((unsigned)stoi(src.substr(pos, 4), nullptr, 16));
                            pos += 4;
                            break;
                        }
                        default:   s += e;    break;   // 未识别：保留字符本身
                    }
                    continue;
                }
                s += src[pos++];
            }
            if (pos >= src.size())
                throw runtime_error(atLex("字符串字面量未闭合（缺少收尾引号 " + string(1, q) + "）"));
            pos++;   // 收尾引号
            return {TK_STR, s, 0, false};
        }
        if (isdigit(c) || (c == '.' && pos+1 < src.size() && isdigit(src[pos+1]))) {
            // ★ P2-3：0x / 0b 前缀、_ 分隔符、科学计数法
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
            if (id == "do")       return {TK_DO, id};       // ★ P2-4
            if (id == "imp")      return {TK_IMP, id};      // ★ 导入原生库
            if (id == "try")      return {TK_TRY, id};      // ★ 错误捕获
            if (id == "catch")    return {TK_CATCH, id};
            if (id == "for")      return {TK_FOR, id};      // ★ P1-3
            if (id == "in")       return {TK_IN, id};       // ★ P1-3
            if (id == "break")    return {TK_BREAK, id};
            if (id == "continue") return {TK_CONTINUE, id};
            if (id == "return")   return {TK_RETURN, id};
            if (id == "local")    return {TK_LOCAL, id};
            if (id == "func")     return {TK_FUNC, id};
            // ★ P1-4：逻辑运算关键字
            if (id == "and")      return {TK_AND, id};
            if (id == "or")       return {TK_OR, id};
            if (id == "not")      return {TK_NOT, id};
            if (id == "true")     return {TK_BOOL, id, 1, false};
            if (id == "false")    return {TK_BOOL, id, 0, false};
            if (id == "null")     return {TK_NULL, id};              // ★
            if (id == "type")     return {TK_IDENT, id};             // ★ 内置
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
        // ★ P2-1 / P2-2：位运算与幂（必须在单字符运算符之前判断）
        if (c == '<' && pos < src.size() && src[pos] == '<') { pos++; return {TK_SHL, "<<", 0, false}; }
        if (c == '>' && pos < src.size() && src[pos] == '>') { pos++; return {TK_SHR, ">>", 0, false}; }
        if (c == '*' && pos < src.size() && src[pos] == '*') { pos++; return {TK_POW, "**", 0, false}; }
        if (c == '&') { return {TK_AMP, "&", 0, false}; }
        if (c == '|') { return {TK_PIPE, "|", 0, false}; }
        if (c == '^') { return {TK_CARET, "^", 0, false}; }
        if (c == '~') { return {TK_TILDE, "~", 0, false}; }
        // ★ 复合赋值必须先于单字符运算符判断（此前词法器从不产出这些 token）
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
            case ';': return {TK_SEMI, ";", 0, false};   // ★ 空语句 / 语句分隔符
            case '.': return {TK_DOT, ".", 0, false};    // ★ 限定库函数调用 io.readFile
            case '?': return {TK_QUESTION, "?", 0, false};   // ★ P1-7
            case ':': return {TK_COLON, ":", 0, false};      // ★ P1-7
            // ★ 非法字符直接报错：此前返回 TK_EOF 会让【剩余全部代码被静默丢弃】
            //   ★ 修：多字节字符要整字打出来（以前只印首字节，终端里显示成乱码 '�'），
            //     中文标识符又是新手最常踩的坑，顺手给一句提示
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

    // ★ P1-1/P1-2：函数体与它的行号表、名字
    struct FuncBlob {
    vector<UpvalDesc> upvals;                  // ★ 1.7 闭包：上值描述表
        vector<uint8_t> code;                      // 只有函数体（不含 FUNC_DEF 头）
        vector<CodeBuf::LineEntry> lines;        // 相对函数体起点
        string   name;
        uint16_t funcId = 0;
        uint16_t paramCount = 0;
        uint16_t localCount = 0;
    };
    vector<FuncBlob> funcDefs;
    vector<pair<uint16_t,string>> funcNames;       // 函数名表（写入字节码，供栈回溯用）
    vector<CodeBuf::LineEntry> allLines;           // 全局行号表
    string srcName = "<source>";                   // 源文件名（file 0）
    vector<string> fileNames;                      // ★ 源文件表（含 .m 模块）
    uint16_t curFileIdx = 0;                       // 当前正在编译的文件
    string   curModule;                            // 非空 = 正在编译这个 .m 模块
    vector<string> moduleStack;                    // 循环导入检测
    // ★ 已加载的 Ae 源码模块：模块名 → (导出函数名 → (funcId, 参数个数))
    struct ModFunc { uint16_t funcId; uint16_t params; };
    map<string, map<string, ModFunc>> modules;
    map<string, ModFunc> impFuncs;                 // 裸名 → 函数（先导入的优先）
    set<string> moduleFiles;
    uint16_t fileIndexFor(const string& path) {
        for (size_t i = 0; i < fileNames.size(); i++) if (fileNames[i] == path) return (uint16_t)i;
        fileNames.push_back(path);
        return (uint16_t)(fileNames.size() - 1);
    }
    uint16_t scriptFuncId = 0;                     // 隐式脚本函数
    set<string> assignedNames;                     // ★ P1-11：全文件出现过的"被赋值的名字"

    // ★ 原生库：名字 → 导出信息（同时登记 "func" 与 "mod.func" 两种写法）
    struct NativeInfo {
        string module;
        string name;
        int    minArgs = 0;
        int    maxArgs = 0;
    };
    map<string, NativeInfo> natives;
    vector<string> importedModules;                  // 写入字节码，VM 启动时加载
    vector<aehost::LoadedModule> loadedModules;      // 保持 DLL 句柄存活
    set<string> importedSet;

    // 读一个库的导出表并登记（第一遍扫描时调用一次）
    void importLibrary(const string& modName) {
        if (importedSet.count(modName)) return;
        aehost::LoadedModule lm;
        vector<string> tried;
        string hint = srcName == "<source>" ? string() : aehost::dirOf(srcName);
        if (!aehost::loadLibrary(modName, hint, lm, tried))
            throw runtime_error(aehost::triedText(modName, tried));
        if (lm.mod->abiVersion != AE_ABI_VERSION)
            throw runtime_error("库 " + modName + " 的 ABI 版本是 " +
                                to_string(lm.mod->abiVersion) + "，本工具链支持 " +
                                to_string(AE_ABI_VERSION) + "（请重新编译该库）");
        int n = 0;
        const AeNativeDef* defs = aehost::readExports(lm, n);
        if (!defs || n <= 0)
            throw runtime_error("库 " + modName + " 没有导出任何函数");
        for (int i = 0; i < n; i++) {
            NativeInfo info;
            info.module  = modName;
            info.name    = defs[i].name;
            info.minArgs = defs[i].minArgs;
            info.maxArgs = defs[i].maxArgs;
            string key = modName + "." + info.name;
            natives[key] = info;                     // 精确调用
            if (!natives.count(info.name)) {          // 裸调用：先导入的库优先
                NativeInfo bare = info;
                bare.module = modName;
                natives[info.name] = bare;
            }
        }
        loadedModules.push_back(lm);
        importedModules.push_back(modName);
        importedSet.insert(modName);
    }
    bool moduleImported(const string& modName) const { return importedSet.count(modName) != 0; }

    // ★ 模块（.m）基础设施
    static bool fileAt(const string& p) { std::ifstream f(p); return (bool)f; }
    vector<string> libSearchDirs() const {
        return aehost::searchDirs(srcName == "<source>" ? string() : aehost::dirOf(srcName));
    }
    bool nativeLibraryExists(const string& name, vector<string>& tried) const {
        for (const string& d : libSearchDirs())
            for (const string& fn : aehost::candidateNames(name)) {
                string p = d + "/" + fn;
                tried.push_back(p);
                if (fileAt(p)) return true;
            }
        return false;
    }
    bool moduleFileExists(const string& name, vector<string>& tried) const {
        for (const string& d : libSearchDirs()) {
            string p = d + "/" + name + ".m";
            tried.push_back(p);
            if (fileAt(p)) return true;
        }
        return false;
    }
    // ★ 编译一个 Ae 源码模块：把它的函数直接编进同一个程序（不需要 ABI、不需要重编工具链）
    void loadModule(const string& name) {
        if (modules.count(name)) return;                 // 已加载
        for (const string& m : moduleStack)
            if (m == name) throw runtime_error("循环导入：'" + name + "' 已经在导入链上了");
        vector<string> tried;
        if (!moduleFileExists(name, tried))
            throw runtime_error("找不到模块文件 " + name + ".m");
        string path;
        for (const string& d : libSearchDirs()) {
            string p = d + "/" + name + ".m";
            if (fileAt(p)) { path = p; break; }
        }
        std::ifstream mf(path, ios::binary);
        if (!mf) throw runtime_error("无法打开模块文件: " + path);
        std::stringstream mss; mss << mf.rdbuf();
        string msrc = mss.str();

        // 保存外层（主文件或上层模块）的编译状态
        string   savedModule = curModule;
        uint16_t savedFile   = curFileIdx;
        bool     savedDecl   = declOnly;
        bool     savedForbid = syms.forbidNewGlobals;
        Lexer    savedL      = L;
        Token    savedCur    = cur;
        vector<Token>            savedQ     = replayed;
        size_t                   savedPos   = replayPos;
        vector<ReplayState>      savedStack = replayStack;

        curModule = name;
        curFileIdx = fileIndexFor(path);
        declOnly = false;                    // 模块的函数体要真正编译
        syms.forbidNewGlobals = true;        // 模块内不许隐式全局
        moduleStack.push_back(name);
        try {
            // ★ 第一遍：登记所有函数签名（模块内部也允许"先用后定义"）
            declOnly = true;
            L.fileIdx = curFileIdx;
            L.reset(msrc);
            validateUtf8();
            replayed.clear(); replayPos = 0; replayStack.clear();
            advance();
            {
                int mdepth = 0;              // ★ 1.7：按深度走，体内的 func 不算声明
                while (cur.type != TK_EOF) {
                    if (cur.type == TK_LBRACE)      { mdepth++; advance(); continue; }
                    if (cur.type == TK_RBRACE)      { if (mdepth > 0) mdepth--; advance(); continue; }
                    if (mdepth == 0 && cur.type == TK_FUNC)     funcDef();
                    else if (mdepth == 0 && cur.type == TK_IMP) impStmt();
                    else advance();
                }
            }
            // ★ 第二遍：真正编译函数体（这里才严格检查"顶层只允许函数与 imp"）
            declOnly = false;
            L.reset(msrc);
            replayed.clear(); replayPos = 0; replayStack.clear();
            advance();
            while (cur.type != TK_EOF) {
                if (cur.type == TK_FUNC)      funcDef();
                else if (cur.type == TK_IMP) { advance(); if (cur.type == TK_IDENT) advance(); }
                else if (cur.type == TK_SEMI) advance();
                else throw runtime_error(at("模块 " + name +
                    " 只允许函数定义和 imp（不允许顶层可执行语句）"));
            }
        } catch (...) {
            moduleStack.pop_back();
            curModule = savedModule; curFileIdx = savedFile; declOnly = savedDecl;
            syms.forbidNewGlobals = savedForbid;
            L = savedL; cur = savedCur; replayed = savedQ; replayPos = savedPos; replayStack = savedStack;
            throw;
        }
        moduleStack.pop_back();
        curModule = savedModule; curFileIdx = savedFile; declOnly = savedDecl;
        syms.forbidNewGlobals = savedForbid;
        L = savedL; cur = savedCur; replayed = savedQ; replayPos = savedPos; replayStack = savedStack;
        modules[name];                       // 登记（可能没有导出函数）
    }
    //   供多赋值判断"整个右侧就是一个调用"并回填 want
    size_t lastCallWantPos = (size_t)-1;
    int    bodyDepth = 0;        // ★ 1.7：当前在几层函数体里（0 = 脚本/模块顶层）
    size_t lastCallEndSize = (size_t)-1;

    // ★ 内置函数 ID：0-6 沿用，7 = type
    static const uint8_t BID_pr    = 0;
    static const uint8_t BID_inp   = 1;
    static const uint8_t BID_prln  = 2;
    static const uint8_t BID_len   = 3;
    static const uint8_t BID_str   = 4;
    static const uint8_t BID_int   = 5;
    static const uint8_t BID_float = 6;
    static const uint8_t BID_type  = 7;   // ★ type()
    // ★ P1-6：错误设施
    static const uint8_t BID_assert = 8;
    static const uint8_t BID_error  = 9;
    static const uint8_t BID_exit   = 10;

    // 通用表指令（须与 VM 一致）
    static const uint8_t OP_NEW_TABLE = 0x50;
    static const uint8_t OP_TABLE_GET = 0x51;
    static const uint8_t OP_TABLE_SET = 0x52;
    static const uint8_t OP_TABLE_KEYS = 0x53;   // ★ P1-3
    static const uint8_t OP_TABLE_LEN  = 0x54;   // ★ P1-3

    uint8_t builtinId(const string& name) const {
        if (name == "pr")     return BID_pr;
        if (name == "inp")    return BID_inp;
        if (name == "prln")   return BID_prln;
        if (name == "len")    return BID_len;
        if (name == "str")    return BID_str;
        if (name == "int")    return BID_int;
        if (name == "float")  return BID_float;
        if (name == "type")   return BID_type;   // ★
        if (name == "assert") return BID_assert; // ★ P1-6
        if (name == "error")  return BID_error;  // ★ P1-6
        if (name == "exit")   return BID_exit;   // ★ P1-6
        // ★ P2-11/12/14/16：字符串 / 表 / 格式化内建
        if (name == "substr")   return 11;
        if (name == "ord")      return 12;
        if (name == "chr")      return 13;
        if (name == "find")     return 14;
        if (name == "upper")    return 15;
        if (name == "lower")    return 16;
        if (name == "trim")     return 17;
        if (name == "replace")  return 18;
        if (name == "split")    return 19;
        if (name == "join")     return 20;
        if (name == "repeat")   return 21;
        if (name == "format")   return 22;
        if (name == "delete")   return 23;
        if (name == "insert")   return 24;
        if (name == "pop")      return 25;
        if (name == "append")   return 26;
        if (name == "count")    return 27;
        if (name == "copy")     return 28;
        if (name == "deepcopy") return 29;
        if (name == "equal")    return 30;
        if (name == "raise")    return 31;      // ★ 错误捕获：重新抛出
        return 0xFF;
    }

    uint16_t nextUserFuncId = 8; // ★ 0-7 留给内置

    bool declOnly = false;
    int  handlerDepth = 0;      // ★ 当前处于几层 try 内部（退出循环/函数时要补 POP_HANDLER）

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


    void pc_reset() {
        L.reset(L.source());
        replayed.clear(); replayPos = 0; replayStack.clear();   // ★ 清掉重放状态
        advance();
    }

    struct LoopCtx {
        size_t startPos;
        int    handlerDepthAtLoop = 0;      // ★ 循环开始时的 try 层数
        uint16_t slotStart = 0;             // ★ 1.7：循环作用域的槽位起点（关上值用）
        vector<size_t> breakPatches;
        vector<size_t> contPatches;
    };
    vector<LoopCtx> loopStack;

    Compiler(const string& srcIn, const string& name = "<source>") : L() {
        srcName = name;
        string src = srcIn;
        // ★ P1-12：跳过 UTF-8 BOM（Windows 编辑器常加）
        if (src.size() >= 3 && (unsigned char)src[0] == 0xEF &&
            (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF)
            src = src.substr(3);
        L.reset(src);
        validateUtf8();
        fileNames.clear();
        fileNames.push_back(srcName);              // file 0 = 主源文件
        curFileIdx = 0;
        L.fileIdx = 0;
        advance();
    }

    // ── ★ token 重放队列 ──
    // 表字面量与下标赋值需要把「已扫描过的 token 序列」当表达式重新解析。
    // 旧实现把 token 拼回源码字符串再用 Lexer 重新词法分析，有两个硬伤：
    //   ① 字符串字面量被退化：{"a b"} 重放成 `a b` → 被当成标识符 a；
    //   ② 依赖 isPunct 猜空白，遇到 "a"+"b" 之类的 token 组合会拼错。
    // 现在直接重放 token 本身，词法与转义解析只做一次。
    vector<Token> replayed;
    size_t        replayPos = 0;

    void advance() {
        // ★ 用「刚被消费掉的 token」的位置落账。
        //   运算符/调用/索引这类指令是在右操作数解析完之后才 emit 的，用 cur
        //   会落到下一个 token（常常已经跨行）→ 波浪线指错地方；
        //   用上一个 token 则正好指向出错的那个操作数（除零时就是除数的位置）。
        if (currentCode) {
            currentCode->pendingLine = (uint32_t)cur.line;
            currentCode->pendingCol  = (uint32_t)cur.col;
            currentCode->pendingFile = curFileIdx;
        }
        if (replayPos < replayed.size()) cur = replayed[replayPos++];
        else cur = L.next();
    }

    // ★ P1-1：把「第 N 行 M 列」贴到诊断信息前面
    string at(const string& msg, int len = 1) const {
        return aePack(curFileIdx, cur.line, cur.col, len, msg);
    }
    // 已越过目标 token 时用这个（带上 token 自己的位置与长度）
    string atPos(int line, int col, int len, const string& msg) const {
        return aePack(curFileIdx, line, col, len, msg);
    }

    // ★ P1-11：读取一个从未在任何地方被赋值的名字 → 编译错误
    //   （`pritn(x)` 这类拼写错误立刻暴露；`x = 1` 仍可隐式创建全局）
    //   注意：这里看的是【全文件出现过的被赋值名字】，所以
    //     · `local z = 1` 出了块之后再读 → 通过检查，运行期读到 null（见 cases/p01_scope.ae，
    //       这是有意保留的行为：作用域外读局部变量得到 null，而不是编译错误）
    //     · 同名的全局/局部互相“掩护”，也是这个机制带来的（改它要连同 golden 一起改）
    void requireDeclared(const string& name) {
        if (assignedNames.count(name)) return;
        if (syms.globals.count(name)) return;
        throw runtime_error(at("未声明的变量 '" + name +
            "'（赋值可隐式创建全局；函数内请先用 local 声明）", (int)name.size()));
    }

    // ★ P1-12：源码必须是合法 UTF-8
    //   词法器按字节处理，GBK 双字节字符的第二字节可能是 0x5C(`\`) 或 0x22(`"`)，
    //   会直接搞坏字符串字面量与转义序列
    void validateUtf8() {
        const string& s = L.source();
        size_t i = 0;
        while (i < s.size()) {
            unsigned char c = (unsigned char)s[i];
            if (c < 0x80) { i++; continue; }
            size_t need;
            if      ((c & 0xE0) == 0xC0) need = 1;
            else if ((c & 0xF0) == 0xE0) need = 2;
            else if ((c & 0xF8) == 0xF0) need = 3;
            else throwBadUtf8(i);
            if (i + need >= s.size()) throwBadUtf8(i);
            for (size_t k = 1; k <= need; k++)
                if (((unsigned char)s[i + k] & 0xC0) != 0x80) throwBadUtf8(i);
            i += need + 1;
        }
    }
    [[noreturn]] void throwBadUtf8(size_t off) {
        int l = 1, c = 1;
        L.lineCol(off, l, c);
        throw runtime_error("第 " + to_string(l) + " 行 " + to_string(c) +
            " 列: 源码不是合法 UTF-8（请把源文件保存为 UTF-8 编码）");
    }

    bool inTry = false;
    //   否则前瞻看到的是源文件的某个位置，与实际取到的 token 不一致
    //   （会报出「期望 (，得到 '*'」这类莫名其妙的错误）。
    // 前瞻第 n 个 token（0 = 下一个），重放期间同样走队列
    Token peekAhead(int n) const {
        size_t avail = (replayPos < replayed.size()) ? (replayed.size() - replayPos) : 0;
        if ((size_t)n < avail) return replayed[replayPos + (size_t)n];
        Lexer tmp = L;
        int skip = n - (int)avail;
        Token last;
        for (int i = 0; i <= skip; i++) last = tmp.next();
        return last;
    }
    Token peekNext() const { return peekAhead(0); }

    void expect(TK t, const string& m) {
        if (cur.type != t) throw runtime_error(at("语法错误: 期望 " + m + "，得到 '" + cur.text + "'", (int)cur.text.size()));
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
    //  通用表字面量: { ... }  →  数组段 + 哈希段
    //   位置元素 {e1, e2}      → 数组段（1-based）
    //   键值对   {k = v} / {[ke] = v} / {t[ke] = v} → 哈希段
    //   可混合： {1, 2, name = "x"}
    // =========================================================================
    struct TableItem {
        bool          isPair;
        vector<Token> keyTokens;   // isPair 时：键表达式的 token 快照
    };

    // 把一个 token 列表当作表达式解析（结果压栈），不干扰外层词法状态。
    // ★ 必须整个快照都用完：否则 `{"k" = 9}` 这类写错的键值对会被静默当成
    //   位置元素（只解析到 "k"，余下的 = 9 被丢掉），产出 null 而不是报错。
    void replayAndCompare(const vector<Token>& toks) {
        replayBegin(toks);
        expr();
        if (cur.type != TK_EOF)
            throw runtime_error(at("表达式没写完，多余的 token '" + cur.text +
                                   "'（键值对要写成 key = value 或 [expr] = value）"));
        replayEnd();
    }

    // ★ P1-5：重放机制拆成 begin/end，便于在重放中直接用指定的 want 调用函数。
    //   ⚠ 保存状态必须用【栈】而不是单个成员：表字面量的值本身可能是表字面量
    //     （{a = {1}}）或含表字面量的调用（f({1})），嵌套重放时单成员会被内层
    //     覆盖，导致外层 replayEnd 恢复出错误的队列 → 解析到哨兵 EOF 就"期望 }"。
    struct ReplayState {
        vector<Token> q;
        size_t        pos;
        Lexer         l;
        Token         c;
    };
    vector<ReplayState> replayStack;

    void replayBegin(const vector<Token>& toks) {
        // ★ 修：`{a = }` / `{, 1}` 这类写法以前报「内部错误：空表达式」，看着像编译器
        //   崩了，其实是普通的语法错误 —— 改成正常的语法诊断（E1001）
        if (toks.empty())
            throw runtime_error(at("语法错误: 这里缺少表达式（常见原因是 `key = ` 后面没写值，"
                                   "或多打了一个逗号）"));
        ReplayState st;
        st.q = replayed; st.pos = replayPos; st.l = L; st.c = cur;
        replayStack.push_back(std::move(st));

        replayed = toks;
        replayed.push_back(Token(TK_EOF, ""));   // 哨兵：队列耗尽即表达式结束
        replayPos = 0;
        advance();                               // cur = toks[0]
    }
    void replayEnd() {
        if (replayStack.empty()) throw runtime_error("内部错误：重放栈不平衡");
        ReplayState st = replayStack.back();
        replayStack.pop_back();
        replayed = st.q; replayPos = st.pos; L = st.l; cur = st.c;
    }

    void tableLiteral() {
        // 表字面量 { ... }：数组段 + 哈希段合一
        //
        // 指令约定（表始终在栈顶）：
        //   NEW_TABLE total  → 压入空表
        //   值; 键; TABLE_SET → 弹出(键,值,表)，表回到栈顶
        // 为保证"值在下、键在上"，用中转槽暂存值。
        //
        // 逐项解析（编译期即可判定结构）：
        //   前瞻 [expr] = v       → 键值对，键 = expr
        //   前瞻 ident = v        → 键值对，键 = 字符串 ident
        //   前瞻 ident[ke] = v    → 键值对，键 = (ident 开头的表达式，以 [..] 结尾)
        //   否则                  → 位置元素，下标 = 序号(1-based)
        //
        // 实现：两遍。第一遍只收集 RawItem（isPair + 键 token 快照），不 emit 值；
        //       第二遍统一 emit（建表 + 每个值/键/SET）。
        advance(); // '{'

        struct RawItem {
            bool isPair;
            vector<Token> key;
            vector<Token> value;   // 值表达式 token 快照（含首个 token）
        };
        vector<RawItem> raw;

        // ★ cur 就是当前位置的 token，L 已停在其后。
        //   旧实现从 L 再取两个 token 做前瞻（等于看了 cur 之后第 1、2 个），
        //   于是 `{name = "Ae"}` 被判成位置元素 → 静默产出 {null}。
        auto peekKv = [&]() -> bool {
            if (cur.type == TK_LBRACKET) return true;              // [expr] = v
            if (cur.type == TK_IDENT) {
                Token b = peekNext();                                  // 紧随其后的 token
                return b.type == TK_ASSIGN || b.type == TK_LBRACKET;  // k = v / k[e] = v
            }
            return false;
        };

        // 扫描"值表达式"的全部 token，直到遇到顶层 ',' 或 '}'（考虑嵌套括号平衡）
        auto scanValue = [&](vector<Token>& out) {
            int paren = 0, brace = 0, brack = 0;
            while (cur.type != TK_EOF) {
                if (cur.type == TK_LPAREN) paren++;
                else if (cur.type == TK_RPAREN) { if (paren == 0) break; paren--; }
                else if (cur.type == TK_LBRACE) brace++;
                else if (cur.type == TK_RBRACE) { if (brace == 0) break; brace--; }
                else if (cur.type == TK_LBRACKET) brack++;
                else if (cur.type == TK_RBRACKET) { if (brack == 0) break; brack--; }
                else if (cur.type == TK_COMMA && paren == 0 && brace == 0 && brack == 0) break;
                out.push_back(cur); advance();
            }
        };

        if (cur.type != TK_RBRACE) {
            while (true) {
                RawItem it;
                if (peekKv()) {
                    it.isPair = true;
                    if (cur.type == TK_LBRACKET) {
                        // [key] = value（键表达式可含嵌套方括号）
                        advance(); // '['
                        int depth = 1;
                        while (cur.type != TK_EOF) {
                            if (cur.type == TK_LBRACKET) depth++;
                            else if (cur.type == TK_RBRACKET) { depth--; if (depth == 0) break; }
                            it.key.push_back(cur); advance();
                        }
                        expect(TK_RBRACKET, "]");
                    } else {
                        // ident = value  或  ident[ke] = value
                        if (cur.type != TK_IDENT) throw runtime_error("表键必须是标识符或 [expr]");
                        Token b = peekNext();   // ★ 只看紧随其后的一个 token
                        if (b.type == TK_ASSIGN) {
                            it.key.push_back(Token(TK_STR, cur.text));   // 键 = 字符串
                            advance(); // ident
                        } else {
                            // ident[ke] = value：键表达式 = ident + [ke]（★ 方括号要一起收进来）
                            it.key.push_back(Token(TK_IDENT, cur.text));
                            advance(); // ident
                            if (cur.type != TK_LBRACKET) throw runtime_error("期望 '[' 或 '='");
                            int depth = 0;
                            while (cur.type != TK_EOF) {
                                if (cur.type == TK_LBRACKET) depth++;
                                else if (cur.type == TK_RBRACKET) depth--;
                                it.key.push_back(cur);          // 连括号一起保留，重放才是 ident[ke]
                                bool closed = (depth == 0 && cur.type == TK_RBRACKET);
                                advance();
                                if (closed) break;
                            }
                            if (depth != 0) throw runtime_error("表键缺少 ']'");
                        }
                    }
                    expect(TK_ASSIGN, "=");
                    scanValue(it.value);   // 值表达式 token
                } else {
                    // 位置元素：值表达式 token
                    scanValue(it.value);
                    it.isPair = false;
                }
                raw.push_back(it);
                if (cur.type == TK_COMMA) {
                    advance();
                    if (cur.type == TK_RBRACE) break;   // ★ 允许尾随逗号 {a = 1,}
                    continue;
                }
                break;
            }
        }
        expect(TK_RBRACE, "}");

        // 中转槽（★ P0-1：随作用域回收的临时槽，不注册名字）
        bool isLocal = syms.inFunction;
        uint16_t tmpSlot;
        if (isLocal) {
            tmpSlot = syms.allocScratch();
        } else {
            auto it = syms.globals.find("__table_tmp");
            if (it == syms.globals.end()) it = syms.globals.emplace("__table_tmp", syms.globalCount++).first;
            tmpSlot = it->second;
        }
        auto storeTmp = [&](uint16_t s) { if (isLocal) { currentCode->emit(0x30); currentCode->u16(s); } else { currentCode->emit(0x04); currentCode->u16(s); } };
        auto loadTmp  = [&](uint16_t s) { if (isLocal) { currentCode->emit(0x31); currentCode->u16(s); } else { currentCode->emit(0x05); currentCode->u16(s); } };

        // 建空表
        currentCode->emit(OP_NEW_TABLE);
        currentCode->u16((uint16_t)raw.size());   // [ tbl ]

        for (size_t i = 0; i < raw.size(); i++) {
            // 栈约定：表始终在栈顶。序列为 [ ... tbl ]
            //  1) 计算值 → 压栈        [ ... tbl v ]
            //  2) 暂存值               [ ... tbl ]
            //  3) 计算键 → 压栈        [ ... tbl key ]
            //  4) 取回值               [ ... tbl key v ]
            //  5) TABLE_SET            [ ... tbl ]   （表回到栈顶）
            vector<Token> valTok = raw[i].value;
            replayAndCompare(valTok);   // [ ... tbl v ]

            if (raw[i].isPair) {
                storeTmp(tmpSlot);       // [ ... tbl ]
                vector<Token> keyTok = raw[i].key;
                replayAndCompare(keyTok);   // [ ... tbl key ]
                loadTmp(tmpSlot);        // [ ... tbl key v ]
                currentCode->emit(OP_TABLE_SET);   // [ ... tbl ]
            } else {
                storeTmp(tmpSlot);       // [ ... tbl ]
                currentCode->emit(0x01);   // LOAD_CONST
                currentCode->u16(cp.addInt((int32_t)(i + 1)));   // 数组段下标 = i+1
                loadTmp(tmpSlot);        // [ ... tbl idx v ]
                currentCode->emit(OP_TABLE_SET);   // [ ... tbl ]
            }
        }
        // 循环结束后，表在栈顶 —— 由外层表达式上下文（赋值 / 函数参数 / 语句 POP）消费
    }

    // =========================================================================
    //  索引访问
    // =========================================================================
    enum IndexMode { IDX_READ, IDX_WRITE };

    VarInfo indexLValue(const string& name) {
        VarInfo vi = syms.resolve(name);
        if (vi.kind == VarInfo::LOCAL) {
            currentCode->emit(0x31); currentCode->u16(vi.slot);
        } else {
            if (vi.slot == (uint16_t)-1) vi.slot = syms.allocGlobal(name);
            currentCode->emit(0x05); currentCode->u16(vi.slot);
        }
        advance();                 // 越过变量名
        expect(TK_LBRACKET, "[");
        expr();                 // 索引表达式
        expect(TK_RBRACKET, "]");
        return vi;
    }

    void postfixIndex() {
        while (cur.type == TK_LBRACKET) {
            advance();
            expr();
            expect(TK_RBRACKET, "]");
            currentCode->emit(OP_TABLE_GET);
        }
    }

    // =========================================================================
    //  顶层 / 函数
    // =========================================================================
    // ★ 原生库：imp <库名>
    void impStmt() {
        advance();                                   // 'imp'
        if (cur.type != TK_IDENT)
            throw runtime_error(at("imp 后需要库名，例如 imp io"));
        string modName = cur.text;
        int nameLine = cur.line, nameCol = cur.col;
        advance();
        vector<string> triedNative;
        bool hasNative = nativeLibraryExists(modName, triedNative);
        vector<string> triedMod;
        bool hasMod = moduleFileExists(modName, triedMod);
        if (hasNative && hasMod)
            std::cerr << "[!] warning: '" << modName << "' 同时存在 .dll 与 .m，将使用原生实现（.dll）" << std::endl;
        if (hasNative) { importLibrary(modName); return; }
        if (hasMod)    { loadModule(modName); return; }
        vector<string> all = triedNative;
        all.insert(all.end(), triedMod.begin(), triedMod.end());
        throw runtime_error(atPos(nameLine, nameCol, (int)modName.size(),
            "找不到库 '" + modName + "'（既没有 <名字>.dll 也没有 <名字>.m；已尝试：") +
            [&]{ string s; for (size_t i = 0; i < all.size(); i++) { if (i) s += ", "; s += all[i]; } return s; }() + "）");
    }

    void program() {
        // ── 第一遍：登记函数签名；处理 imp；收集"被赋值的名字"（P1-11）──
        declOnly = true;
        {
            int depth = 0;                       // ★ 1.7：深度 > 0 就是函数体内部，不能当声明
            while (cur.type != TK_EOF) {
                if (cur.type == TK_LBRACE)      { depth++; advance(); continue; }
                if (cur.type == TK_RBRACE)      { if (depth > 0) depth--; advance(); continue; }
                if (depth == 0 && cur.type == TK_FUNC) { funcDef(); continue; }
                if (depth == 0 && cur.type == TK_IMP)  { impStmt(); continue; }
                if (cur.type == TK_IDENT) {
                    Token nx = peekNext();
                    if (nx.type == TK_ASSIGN || nx.type == TK_ADDEQ || nx.type == TK_SUBEQ ||
                        nx.type == TK_MULEQ || nx.type == TK_DIVEQ)
                        assignedNames.insert(cur.text);      // 含 `local x = …`：见 requireDeclared 的说明
                }
                advance();
            }
        }
        declOnly = false;
        scriptFuncId = nextUserFuncId++;   // ★ P1-9：给隐式脚本函数分配一个 id

        // ── 第二遍：顶层语句编译进隐式脚本函数 __script ──
        //    ★ P1-9：于是顶层也能用 local、也能 return；
        //    ★ P1-8：顶层语句执行完后自动调用 main（若定义）
        pc_reset();
        syms.inFunction = true;
        syms.beginFunction();

        CodeBuf scriptBuf;
        CodeBuf* savedCode = currentCode;
        currentCode = &scriptBuf;

        while (cur.type != TK_EOF) {
            if (cur.type == TK_FUNC) {
                // ★ funcDef 会重建符号作用域栈，所以先保存脚本自己的作用域状态
                vector<SymbolTable::Scope> savedScopes = syms.scopes;
                uint16_t savedLocal = syms.localCount, savedMax = syms.maxLocalCount;
                bool savedInFunc = syms.inFunction;
                funcDef();
                syms.scopes      = savedScopes;
                syms.localCount  = savedLocal;
                syms.maxLocalCount = savedMax;
                syms.inFunction  = savedInFunc;
            }
            else if (cur.type == TK_IMP) {
                // 第一遍已经加载过，这里只跳过
                advance();
                if (cur.type == TK_IDENT) advance();
            }
            else stmt();
        }

        auto itMain = syms.functions.find("main");
        if (itMain != syms.functions.end()) {
            uint16_t mid = itMain->second;
            if (syms.funcParamCounts[mid] != 0)
                throw runtime_error("main 不能有参数（入口自动调用时不传参）");
            scriptBuf.markLine(scriptBuf.lines.empty() ? 1 : scriptBuf.lines.back().line);
            scriptBuf.emit(0x41); scriptBuf.u16(mid); scriptBuf.u8(0); scriptBuf.u8(0);  // CALL main argc=0 want=0
        }
        scriptBuf.markLine(scriptBuf.lines.empty() ? 1 : scriptBuf.lines.back().line);
        scriptBuf.emit(0x42); scriptBuf.u8(0);      // RET 0

        currentCode = savedCode;
        uint16_t scriptLocals = syms.maxLocalCount;
        syms.inFunction = false;
        syms.endFunction();

        // ── 入口 stub：CALL __script; HALT ──
        output.markLine(1);
        output.emit(0x41); output.u16(scriptFuncId); output.u8(0); output.u8(0);
        output.emit(0x03); // HALT

        // 函数定义：脚本函数放最前，其后是用户函数
        FuncBlob sb;
        sb.code = scriptBuf.code;
        sb.lines = scriptBuf.lines;
        sb.name = "<script>";
        sb.funcId = scriptFuncId;
        sb.paramCount = 0;
        sb.localCount = scriptLocals;
        emitFuncBlob(sb);

        for (const FuncBlob& f : funcDefs) emitFuncBlob(f);

        for (const auto& L : output.lines) allLines.push_back(L);
        sort(allLines.begin(), allLines.end(), [](const CodeBuf::LineEntry& a, const CodeBuf::LineEntry& b) { return a.off < b.off; });
    }

    // 输出一个函数定义（FUNC_DEF 头 + 函数体），并把它的行号表重定位到 output
    void emitFuncBlob(const FuncBlob& f) {
        // ★ 1.7：头 = 1+2+2+2+4 再 +2(上值个数) +3×上值个数
        uint32_t bodyStart = (uint32_t)output.size() + 11 + 2 + (uint32_t)(3 * f.upvals.size());
        output.emit(0x40);
        output.u16(f.funcId);
        output.u16(f.paramCount);
        output.u16(f.localCount);
        output.u32((uint32_t)f.code.size());
        output.u16((uint16_t)f.upvals.size());
        for (const UpvalDesc& d : f.upvals) {
            output.u8(d.fromParentLocal ? 1 : 0);
            output.u16(d.index);
        }
        for (uint8_t b : f.code) output.emit(b);
        for (const auto& L : f.lines)
            allLines.push_back({L.off + bodyStart, L.line, L.col, L.file});
        funcNames.push_back({f.funcId, f.name});
    }

    void funcDef() {
        advance(); // 'func'

        if (cur.type != TK_IDENT) throw runtime_error("func 后需要函数名");
        string fname = cur.text;
        advance();

        // ★ 1.7：写在【函数体里面】的 func name(...) 是局部闭包（能捕获外层变量），
        //   等价于 local name = func name(...) {...}。
        //   顶层 / 模块顶层的 func 声明才是全局静态函数（没有外层可捕获）。
        if (bodyDepth > 0) {
            uint16_t slot = syms.allocLocal(fname);
            uint8_t  nup  = 0;
            uint16_t fid  = compileClosureBody(fname, &nup);
            currentCode->emit(0x44); currentCode->u16(fid); currentCode->u8(nup);
            currentCode->emit(0x30); currentCode->u16(slot);      // 存进局部槽
            return;
        }

        // ★ 模块内的函数在符号表里用 "模块名.函数名"，避免与主文件/其他模块撞名
        string key = curModule.empty() ? fname : (curModule + "." + fname);
        uint16_t funcId;
        if (declOnly && syms.functions.count(key))
            throw runtime_error(at("函数 '" + fname + "' 重复定义"));
        if (curModule.empty() && builtinId(fname) != 0xFF)
            throw runtime_error(at("'" + fname + "' 是内建函数名，不能用来定义函数"));
        auto it = syms.functions.find(key);
        if (it != syms.functions.end()) funcId = it->second;
        else {
            funcId = nextUserFuncId++;
            syms.functions[key] = funcId;
        }

        expect(TK_LPAREN, "(");
        syms.inFunction = true;
        syms.beginFunction();          // ★ P0-1：建立函数作用域

        vector<string> params;
        if (cur.type != TK_RPAREN) {
            if (cur.type != TK_IDENT) throw runtime_error("参数名必须是标识符");
            params.push_back(cur.text); syms.allocParam(cur.text); advance();
            while (cur.type == TK_COMMA) {
                advance();
                if (cur.type != TK_IDENT) throw runtime_error("参数名必须是标识符");
                params.push_back(cur.text); syms.allocParam(cur.text); advance();
            }
        }
        syms.funcParamCounts[funcId] = (uint16_t)params.size();
        expect(TK_RPAREN, ")");

        // ★ 1.7：第一遍必须在【吃掉 '{' 之前】返回，把函数体留给调用方的
        //   "按花括号深度遍历"去跳过。两个要求同时满足：
        //     · 体内的 func（闭包表达式 / 嵌套声明）不会被误当成顶层函数声明
        //     · 体内被赋值的名字仍能被扫到 → P1-11 的拼写检查不会误报
        if (declOnly) {
            syms.inFunction = false;
            syms.endFunction();
            return;
        }

        expect(TK_LBRACE, "{");

        CodeBuf bodyBuf;
        CodeBuf* savedCode = currentCode;
        int savedHandlerDepth = handlerDepth;        // ★ 函数体有自己的 try 层数
        handlerDepth = 0;
        int savedBodyDepth = bodyDepth;              // ★ 1.7：函数体里的 func 声明 = 局部闭包
        bodyDepth = savedBodyDepth + 1;
        currentCode = &bodyBuf;
        bodyBuf.markLine((uint32_t)cur.line);       // ★ P1-1：函数体起始行

        while (cur.type != TK_RBRACE && cur.type != TK_EOF) stmt();
        expect(TK_RBRACE, "}");

        // ★ P1-10：函数体末尾统一补一条 RET 0（返回"无值"）。
        //   若最后一条语句本身就是 return，这条是死代码，无害。
        bodyBuf.emit(0x42);
        bodyBuf.u8(0);

        currentCode = savedCode;
        // ★ P0-1：帧大小取【高水位】，因为嵌套块退出后水位会回退复用槽位，
        //   而运行期某个时刻可能用到过更深的槽位
        uint16_t frameLocals = syms.maxLocalCount;
        handlerDepth = savedHandlerDepth;
        bodyDepth    = savedBodyDepth;
        syms.inFunction = false;
        syms.endFunction();

        // ★ 导出：非 _ 开头的函数才能被 imp 它的文件调用
        if (!curModule.empty() && !fname.empty() && fname[0] != '_') {
            ModFunc mf; mf.funcId = funcId; mf.params = (uint16_t)params.size();
            modules[curModule][fname] = mf;
            if (!impFuncs.count(fname)) impFuncs[fname] = mf;
        }
        FuncBlob fb;
        fb.code       = bodyBuf.code;
        fb.lines      = bodyBuf.lines;
        fb.name       = key;          // ★ 模块函数显示为 "json.parse"，诊断更清楚
        fb.funcId     = funcId;
        fb.paramCount = (uint16_t)params.size();
        fb.localCount = frameLocals;
        funcDefs.push_back(fb);
    }

    // =========================================================================
    //  语句
    // =========================================================================
    // =========================================================================
    //  ★ P0-1：块级作用域
    //  每个受控体（{...} 块、if/else 体、while 体）都是一个独立作用域：
    //  进入时 push、退出时 pop（回收槽位）。于是
    //    · 内层 `local x` 遮蔽外层 x，不再改写它
    //    · 块内声明的 local 出块即不可见
    // =========================================================================
    void scopedBlock() {
        syms.pushScope();
        if (cur.type == TK_LBRACE) {
            advance();
            while (cur.type != TK_RBRACE && cur.type != TK_EOF) stmt();
            if (cur.type == TK_RBRACE) advance();
        } else {
            stmt();
        }
        endScope();          // ★ 1.7：块退出时关掉块内局部变量的上值
    }

    // ★ 1.7：作用域退出时，把这段槽位上"打开的上值"关掉。
    //   不关的话，下一轮循环（或下次进入这个块）创建的新闭包会继续共享同一个变量 ——
    //   表现就是"循环里建的 9 个回调全都拿到最后一个值"。没有上值打开时这条指令只是
    //   空转（很快），所以不必先分析"这个变量到底被捕获了没有"。
    void emitCloseScope(uint16_t from, uint16_t to) {
        if (to <= from) return;
        for (uint16_t s = from; s < to; s++) { currentCode->emit(0x47); currentCode->u16(s); }
    }
    void endScope() {
        if (!syms.scopes.empty())
            emitCloseScope(syms.scopes.back().startSlot, syms.localCount);
        syms.popScope();
    }

    void stmt() {

        currentCode->markLine((uint32_t)cur.line);   // ★ P1-1：语句边界记行号
        if (cur.type == TK_SEMI) {          // ★ 空语句 / 语句分隔符
            advance();
            return;
        }
        if (cur.type == TK_IF) {
            ifStmt();
        } else if (cur.type == TK_FOR) {
            forStmt();                              // ★ P1-3
        } else if (cur.type == TK_WHILE) {
            whileStmt();
        } else if (cur.type == TK_BREAK) {
            loopJumpStmt(true);
        } else if (cur.type == TK_CONTINUE) {
            loopJumpStmt(false);
        } else if (cur.type == TK_DO) {
            doWhileStmt();                              // ★ P2-4
        } else if (cur.type == TK_IMP) {
            // 库在第一遍已加载（函数体内也不会重复写字节码）
            advance();
            if (cur.type == TK_IDENT) advance();
        } else if (cur.type == TK_TRY) {
            tryStmt();
        } else if (cur.type == TK_RETURN) {
            returnStmt();
        } else if (cur.type == TK_LOCAL) {
            localDecl();
        } else if (cur.type == TK_LBRACE) {
            scopedBlock();                 // ★ P0-1：块有自己的作用域
        } else {
            exprStmt();
        }
    }

    void exprStmt() {
        if (cur.type == TK_IDENT) {
            string name = cur.text;
            Token peek = peekNext();
            // ★ 点号开头的语句：库调用 / 表字段赋值 / 字段读取
            if (peek.type == TK_DOT) {
                Token fld = peekAhead(1);
                if (fld.type == TK_IDENT && isModuleFunction(name, fld.text)) {
                    callExpr(0);                             // io.writeFile(...) / json.encode(...)
                    return;
                }
                Token after = peekAhead(2);
                if (fld.type == TK_IDENT && after.type == TK_ASSIGN) {
                    fieldAssignStmt(name, fld.text);         // t.a = v
                    return;
                }
                if (fld.type == TK_IDENT && (after.type == TK_ADDEQ || after.type == TK_SUBEQ ||
                                             after.type == TK_MULEQ || after.type == TK_DIVEQ)) {
                    fieldCompoundStmt(name, fld.text, after.type);   // t.a += v
                    return;
                }
                compare(false);                              // 其余当表达式语句
                currentCode->emit(0x43);
                return;
            }
            // ★ P1-5：多赋值  a, b, c = ...
            if (peek.type == TK_COMMA) {
                vector<string> names;
                for (;;) {
                    if (cur.type != TK_IDENT)
                        throw runtime_error(at("多赋值的目标必须是变量名（暂不支持 t[i] 形式）"));
                    names.push_back(cur.text);
                    advance();
                    if (cur.type == TK_COMMA) { advance(); continue; }
                    break;
                }
                expect(TK_ASSIGN, "=");
                emitMultiAssign(names, false);
                return;
            }

            if (peek.type == TK_ASSIGN) {
                advance(); advance();
                expr();
                emitStoreVar(name);              // ★ 闭包：LOCAL / UPVAL / GLOBAL 统一
                return;
            } else if (peek.type == TK_LBRACKET) {
                handleIndexStatement(name);
                return;
            } else if (peek.type == TK_ADDEQ || peek.type == TK_SUBEQ ||
                       peek.type == TK_MULEQ || peek.type == TK_DIVEQ) {
                advance();
                TK op = cur.type; advance();
                emitLoadVar(name, true);          // ★ 闭包：读（LOCAL / UPVAL / GLOBAL）
                expr();
                switch (op) {
                    case TK_ADDEQ: currentCode->emit(0x0A); break;
                    case TK_SUBEQ: currentCode->emit(0x0B); break;
                    case TK_MULEQ: currentCode->emit(0x0C); break;
                    case TK_DIVEQ: currentCode->emit(0x0D); break;
                    default: break;
                }
                emitStoreVar(name);               // ★ 闭包：写
                return;
            }
            if (peek.type == TK_LPAREN) {
                callExpr(0);        // ★ P1-5：语句上下文 → 丢弃全部返回值
                return;
            }
        }
        expr();
        currentCode->emit(0x43);   // ★ POP：表达式语句丢弃结果，操作数栈不再泄漏
    }

    void handleIndexStatement(const string& name) {
        // 解析：name [key1] [key2] ... [keyn]  （可链式，keyn 可为任意表达式）
        // 先加载 name，再逐个压入 key 并 GET；记录每一级的 key 表达式 token，
        // 因为“值在下、键在上”的 SET 约定需要先算值 —— 采用中转槽方案。
        VarInfo vi = syms.resolve(name);
        if (vi.kind == VarInfo::LOCAL) { currentCode->emit(0x31); currentCode->u16(vi.slot); }
            else if (vi.kind == VarInfo::UPVAL) { currentCode->emit(0x45); currentCode->u16(vi.slot); }   // ★ 闭包：上值
        else {
            if (vi.slot == (uint16_t)-1) vi.slot = syms.allocGlobal(name);
            currentCode->emit(0x05); currentCode->u16(vi.slot);
        }
        advance(); // name

        // 收集所有 [key]（记录 key 的 token 序列，用于“值在下键在上”的赋值）
        vector<vector<Token>> keys;
        while (cur.type == TK_LBRACKET) {
            advance(); // '['
            vector<Token> kt;
            // 保存从 '[' 后到 ']' 前的所有 token
            while (cur.type != TK_RBRACKET && cur.type != TK_EOF) {
                kt.push_back(cur); advance();
            }
            expect(TK_RBRACKET, "]");
            keys.push_back(kt);
        }

        if (keys.empty()) return;   // 不应发生（调用方已保证）

        bool isLocal = syms.inFunction;
        uint16_t tmpSlot;
        if (isLocal) {
            tmpSlot = syms.allocScratch();     // ★ P0-1：临时槽
        } else {
            // ★ 修复：此前把临时槽写成 0xFFFF，VM 会 STORE 到 slot 65535 →
            //   globals 被扩容到 65536 项。改为正常分配一个隐藏全局槽。
            auto it = syms.globals.find("__tbl_idx_tmp");
            if (it == syms.globals.end())
                it = syms.globals.emplace("__tbl_idx_tmp", syms.globalCount++).first;
            tmpSlot = it->second;
        }
        auto storeTmp = [&]() {
            if (isLocal) { currentCode->emit(0x30); currentCode->u16(tmpSlot); }
            else         { currentCode->emit(0x04); currentCode->u16(tmpSlot); }
        };
        auto loadTmp = [&]() {
            if (isLocal) { currentCode->emit(0x31); currentCode->u16(tmpSlot); }
            else         { currentCode->emit(0x05); currentCode->u16(tmpSlot); }
        };

        if (cur.type == TK_ASSIGN) {
            // ── 链式赋值：name[k1]...[kn] = rhs ──
            // 栈演化（表始终在顶）：
            //   下钻前 n-1 级：每级 emit key; TABLE_GET
            for (size_t i = 0; i + 1 < keys.size(); i++) {
                replayAndCompare(keys[i]);   // key_i
                currentCode->emit(OP_TABLE_GET);   // → tbl_i
            }
            // 最后一级：值 → tmp，键 → 栈顶，SET
            advance(); // '='
            expr();   // rhs 值
            storeTmp();      // 值存入 tmp
            replayAndCompare(keys.back());   // 最后一级键
            loadTmp();       // 值回到栈顶
            currentCode->emit(OP_TABLE_SET);   // 表回到栈顶（丢弃，语句结束）
            currentCode->emit(0x43); // POP 表（表达式语句不关心返回值）
        } else {
            // ── 读取：逐级 GET，最后一级多一次 GET（栈顶即结果）──
            for (size_t i = 0; i < keys.size(); i++) {
                replayAndCompare(keys[i]);
                currentCode->emit(OP_TABLE_GET);
            }
            // ★ 1.7：下标/字段取值后直接调用（handlers[i]() / t.f()）—— 语句位置也要支持
            if (cur.type == TK_LPAREN) emitDynamicCall(1);
            currentCode->emit(0x43); // POP 最终结果（语句上下文）
        }
    }

    // ★ 表字段赋值 t.a = v（等价 t["a"] = v）
    void fieldAssignStmt(const string& base, const string& fld) {
        advance();                                   // 越过基名
        expect(TK_DOT, ".");
        if (cur.type != TK_IDENT) throw runtime_error(at("点号后需要字段名"));
        advance();                                   // 越过字段名
        expect(TK_ASSIGN, "=");
        uint16_t tmp = syms.allocScratch();
        emitLoadVar(base, true);                     // [t]
        expr();                                      // [t v]
        genStore(tmp);                               // [t]
        currentCode->emit(0x01); currentCode->u16(cp.addString(fld));   // [t k]
        genLoad(tmp);                                // [t k v]
        currentCode->emit(OP_TABLE_SET);             // [t]
        currentCode->emit(0x43);                     // POP 语句结束
    }

    // ★ 表字段复合赋值 t.a += v（json 这类解析器大量使用）
    void fieldCompoundStmt(const string& base, const string& fld, TK op) {
        advance();                                   // 基名
        expect(TK_DOT, ".");
        if (cur.type != TK_IDENT) throw runtime_error(at("点号后需要字段名"));
        advance();                                   // 字段名
        advance();                                   // 复合赋值运算符
        uint16_t tmp = syms.allocScratch();
        emitLoadVar(base, true);
        currentCode->emit(0x01); currentCode->u16(cp.addString(fld));
        currentCode->emit(OP_TABLE_GET);             // [old]
        expr();                                      // [old rhs]
        switch (op) {
            case TK_ADDEQ: currentCode->emit(0x0A); break;
            case TK_SUBEQ: currentCode->emit(0x0B); break;
            case TK_MULEQ: currentCode->emit(0x0C); break;
            case TK_DIVEQ: currentCode->emit(0x0D); break;
            default: break;
        }                                            // [new]
        genStore(tmp);
        emitLoadVar(base, true);
        currentCode->emit(0x01); currentCode->u16(cp.addString(fld));
        genLoad(tmp);
        currentCode->emit(OP_TABLE_SET);             // [t]
        currentCode->emit(0x43);                     // POP
    }

    void localDecl() {
        if (!syms.inFunction) throw runtime_error(at("local 只能在函数内使用"));
        advance(); // 'local'
        vector<string> names;
        for (;;) {
            if (cur.type != TK_IDENT) throw runtime_error(at("local 后需要变量名"));
            names.push_back(cur.text);
            advance();
            if (cur.type == TK_COMMA) { advance(); continue; }
            break;
        }
        expect(TK_ASSIGN, "=");
        emitMultiAssign(names, true);
    }

    // ── ★ P1-5：多赋值 / 多声明 ──
    //   local a, b = f()          → f 按 want=2 调用
    //   local a, b = 1, f()       → f 按 want=1
    //   local a, b = 1            → 第二个补 null
    //   a, b = b, a               → 交换
    //   实现要点：表达式边界直接用"代码缓冲区长度"判定，不做 token 前瞻
    //   （本语言没有语句终结符，靠 token 猜表达式结尾是不可靠的）
    void emitMultiAssign(const vector<string>& names, bool declare) {
        size_t n = names.size();

        struct RhsInfo { size_t wantPos; bool wholeCall; };
        vector<RhsInfo> infos;
        size_t m = 0;

        for (;;) {
            size_t before = currentCode->size();
            lastCallWantPos = (size_t)-1;
            lastCallEndSize = (size_t)-1;
            expr();
            size_t after = currentCode->size();
            bool wholeCall = (lastCallEndSize == after && lastCallEndSize != (size_t)-1 &&
                              lastCallEndSize > before);
            infos.push_back({lastCallWantPos, wholeCall});
            m++;
            if (cur.type == TK_COMMA) { advance(); continue; }
            break;
        }
        if (m > n)
            throw runtime_error(at("赋值右侧有 " + to_string(m) +
                " 个表达式，多于左侧 " + to_string(n) + " 个目标"));

        size_t remaining = n - (m - 1);          // 最后一个表达式需要产出的个数
        size_t produced  = m - 1;
        if (infos.back().wholeCall) {
            // 整个右侧末项就是一个函数调用 → 直接改掉它的 want，取多值
            currentCode->patchU8At(infos.back().wantPos,
                                   (uint8_t)std::min<size_t>(remaining, 255));
            produced += remaining;
        } else {
            produced += 1;
        }
        // 目标多于实得值 → 用 null 补齐（Lua 语义：未提供的目标为 nil）
        for (size_t i = produced; i < n; i++) {
            currentCode->emit(0x01);
            currentCode->u16(cp.addNull());
        }

        // 从右向左存入目标（此时栈顶恰好 n 个值，顺序为左→右）
        for (size_t i = n; i-- > 0; ) {
            const string& nm = names[i];
            if (declare) {
                uint16_t slot = syms.allocLocal(nm);       // 先求值后声明 → `local x = x` 取外层值
                currentCode->emit(0x30); currentCode->u16(slot);
            } else {
                VarInfo vi = syms.resolve(nm);
                if (vi.kind == VarInfo::LOCAL) { currentCode->emit(0x30); currentCode->u16(vi.slot); }
            else if (vi.kind == VarInfo::UPVAL) { currentCode->emit(0x46); currentCode->u16(vi.slot); }   // ★ 闭包：上值
                else {
                    if (vi.slot == (uint16_t)-1) vi.slot = syms.allocGlobal(nm);
                    currentCode->emit(0x04); currentCode->u16(vi.slot);
                }
            }
        }
    }

    void ifStmt() {
        advance(); // 'if'
        expect(TK_LPAREN, "(");
        expr();
        expect(TK_RPAREN, ")");
        currentCode->emit(0x2A);
        size_t jzPos = placeholderU16();
        scopedBlock();                     // ★ P0-1
        if (cur.type == TK_ELSE) {
            advance();
            currentCode->emit(0x2B);
            size_t jmpPos = placeholderU16();
            size_t elseStart = currentCode->size();
            patchU16(jzPos, jmpOffset(jzPos, elseStart));
            scopedBlock();                 // ★ P0-1
            size_t endPos = currentCode->size();
            patchU16(jmpPos, jmpOffset(jmpPos, endPos));
        } else {
            size_t endPos = currentCode->size();
            patchU16(jzPos, jmpOffset(jzPos, endPos));
        }
    }

    // =========================================================================
    //  ★ P1-3：for 循环
    //    数值：for i = start, stop [, step] { ... }   （stop 含端点，C 风格可改 i）
    //    遍历：for k, v in t { ... } / for v in t { ... }
    //      · 顺序确定：先数组段 1..n，再哈希段按 TableKey 排序
    //      · 快照语义：遍历期间增删键不影响本次遍历
    //      · 值为 null 的槽不出现（与 Lua pairs 跳过 nil 一致）
    // =========================================================================
    void forStmt() {
        advance(); // 'for'
        if (cur.type != TK_IDENT) throw runtime_error(at("for 后需要循环变量名"));
        vector<string> vars;
        vars.push_back(cur.text);
        advance();
        while (cur.type == TK_COMMA) {
            advance();
            if (cur.type != TK_IDENT) throw runtime_error(at("for 后需要循环变量名"));
            vars.push_back(cur.text);
            advance();
        }
        if (vars.size() > 2) throw runtime_error(at("for 最多支持 2 个循环变量"));

        bool generic = (cur.type == TK_IN);
        if (!generic && vars.size() != 1)
            throw runtime_error(at("数值 for 只接受 1 个循环变量：for i = a, b"));
        if (generic) {
            if (cur.type != TK_IN) throw runtime_error(at("期望 'in'"));
            advance();
        } else {
            expect(TK_ASSIGN, "=");
        }

        syms.pushScope();                        // 循环变量只在循环内可见

        if (generic) {
            // ── for k, v in t ──
            uint16_t tSlot    = syms.allocScratch();
            uint16_t keysSlot = syms.allocScratch();
            uint16_t cntSlot  = syms.allocScratch();
            uint16_t curSlot  = syms.allocScratch();
            uint16_t hiddenK  = syms.allocScratch();
            uint16_t vSlot    = syms.allocLocal(vars.back());
            uint16_t kSlot    = (vars.size() == 2) ? syms.allocLocal(vars[0]) : hiddenK;

            expr();
            genStore(tSlot);
            genLoad(tSlot); currentCode->emit(OP_TABLE_KEYS); genStore(keysSlot);
            genLoad(keysSlot); currentCode->emit(OP_TABLE_LEN); genStore(cntSlot);
            currentCode->emit(0x01); currentCode->u16(cp.addInt(1)); genStore(curSlot);

            LoopCtx ctx;
            ctx.slotStart = kSlot;      // ★ 本循环的槽位起点 = k 那一格（v 与体内局部都在上面）
            loopStack.push_back(ctx);
            size_t loopStart = currentCode->size();
            genLoad(curSlot); genLoad(cntSlot); currentCode->emit(0x13);   // cursor <= count
            currentCode->emit(0x2A);
            size_t jzEnd = placeholderU16();
            genLoad(keysSlot); genLoad(curSlot); currentCode->emit(OP_TABLE_GET); genStore(kSlot);
            genLoad(tSlot);    genLoad(kSlot);   currentCode->emit(OP_TABLE_GET); genStore(vSlot);
            scopedBlock();
            // ★ 1.7：用本轮的 k/v 关上值，然后再移动游标；continue 落点也在这之后
            emitCloseScope(syms.scopes.back().startSlot, syms.localCount);
            size_t incPos = currentCode->size();
            for (size_t pos : loopStack.back().contPatches) patchU16(pos, jmpOffset(pos, incPos));
            genLoad(curSlot); currentCode->emit(0x01); currentCode->u16(cp.addInt(1));
            currentCode->emit(0x06);                                        // cursor += 1
            genStore(curSlot);
            currentCode->emit(0x2B);
            size_t jmpBack = placeholderU16();
            size_t loopEnd = currentCode->size();
            for (size_t pos : loopStack.back().breakPatches) patchU16(pos, jmpOffset(pos, loopEnd));
            loopStack.pop_back();
            patchU16(jzEnd, jmpOffset(jzEnd, loopEnd));
            patchU16(jmpBack, jmpOffset(jmpBack, loopStart));
            endScope();
            return;
        }

        // ── 数值 for：先把边界算进临时槽，再声明循环变量 ──
        uint16_t startTmp = syms.allocScratch();
        uint16_t stopSlot = syms.allocScratch();
        uint16_t stepSlot = syms.allocScratch();
        uint16_t dirSlot  = syms.allocScratch();
        expr(); genStore(startTmp);
        expect(TK_COMMA, ",");
        expr(); genStore(stopSlot);
        if (cur.type == TK_COMMA) {
            advance();
            expr(); genStore(stepSlot);
        } else {
            currentCode->emit(0x01); currentCode->u16(cp.addInt(1)); genStore(stepSlot);
        }
        uint16_t iSlot = syms.allocLocal(vars[0]);
        genLoad(startTmp); genStore(iSlot);

        // 步长为 0 → 直接报错；同时把方向（step > 0）算一次存起来
        genLoad(stepSlot);
        currentCode->emit(0x02); currentCode->u8(BID_float); currentCode->u8(1); currentCode->u8(1);
        currentCode->emit(0x01); currentCode->u16(cp.addDouble(0.0));
        currentCode->emit(0x10);                                   // EQ → step == 0 ?
        currentCode->emit(0x2A);
        size_t jzOk = placeholderU16();
        currentCode->emit(0x55);
        currentCode->u16(cp.addString("for 步长不能为 0（会导致死循环）"));   // THROW_CONST
        patchU16(jzOk, jmpOffset(jzOk, currentCode->size()));

        genLoad(stepSlot);
        currentCode->emit(0x02); currentCode->u8(BID_float); currentCode->u8(1); currentCode->u8(1);
        currentCode->emit(0x01); currentCode->u16(cp.addDouble(0.0));
        currentCode->emit(0x14);                                   // GT → step > 0
        genStore(dirSlot);

        LoopCtx ctx;
        ctx.handlerDepthAtLoop = handlerDepth;
        ctx.slotStart = iSlot;          // ★ 本循环的槽位起点 = 循环变量那一格
        loopStack.push_back(ctx);
        size_t loopStart = currentCode->size();
        genLoad(dirSlot);
        currentCode->emit(0x2A);
        size_t jzNeg = placeholderU16();
        genLoad(iSlot); genLoad(stopSlot); currentCode->emit(0x13);   // 正步长：i <= stop
        currentCode->emit(0x2B);
        size_t jmpCmp = placeholderU16();
        patchU16(jzNeg, jmpOffset(jzNeg, currentCode->size()));
        genLoad(iSlot); genLoad(stopSlot); currentCode->emit(0x15);   // 负步长：i >= stop
        patchU16(jmpCmp, jmpOffset(jmpCmp, currentCode->size()));
        currentCode->emit(0x2A);
        size_t jzEnd = placeholderU16();

        scopedBlock();

        // ★ 1.7：先用【本轮】的值把循环变量的上值关掉，再自增（顺序反了闭包会看到 k+1）。
        //   continue 的跳转目标在下一句，所以 continue 也走同一条收尾。
        //   ★ 范围只能从【本循环的槽位起点】往上！从 0 开始会把外层函数的变量也关掉，
        //     而那些变量（比如事件里累加的计数器）本该和外层共享同一个 cell ——
        //     关掉之后 handler 只在改自己的副本，外面永远不变（实测"点按钮计数不增加"）。
        emitCloseScope(loopStack.back().slotStart, syms.localCount);
        size_t incPos = currentCode->size();
        for (size_t pos : loopStack.back().contPatches) patchU16(pos, jmpOffset(pos, incPos));
        genLoad(iSlot); genLoad(stepSlot); currentCode->emit(0x0A);   // i = i + step
        genStore(iSlot);
        currentCode->emit(0x2B);
        size_t jmpBack = placeholderU16();
        size_t loopEnd = currentCode->size();
        for (size_t pos : loopStack.back().breakPatches) patchU16(pos, jmpOffset(pos, loopEnd));
        loopStack.pop_back();
        patchU16(jzEnd, jmpOffset(jzEnd, loopEnd));
        patchU16(jmpBack, jmpOffset(jmpBack, loopStart));
        syms.popScope();
    }

    // ★ P2-5：break / continue，可带层数：break 2 表示作用于外层第 2 层循环
    void loopJumpStmt(bool isBreak) {
        advance(); // 'break' / 'continue'
        size_t level = 1;
        if (cur.type == TK_NUM && !cur.isFloat) {
            if (cur.ival < 1) throw runtime_error(at("break/continue 的层数必须 >= 1"));
            level = (size_t)cur.ival;
            advance();
        }
        if (loopStack.empty())
            throw runtime_error(at(isBreak ? "break 只能在循环体内使用" : "continue 只能在循环体内使用"));
        if (level > loopStack.size())
            throw runtime_error(at((isBreak ? "break " : "continue ") + to_string(level) +
                " 超出当前循环嵌套深度 " + to_string(loopStack.size())));
        LoopCtx& lc = loopStack[loopStack.size() - level];
        for (int i = lc.handlerDepthAtLoop; i < handlerDepth; i++)
            currentCode->emit(0x28);                   // ★ 跳出 try 时要注销处理器
        // ★ 1.7：跳出前关掉【本循环范围内】的上值（continue 会让下一轮的闭包共享这一轮
        //   的变量）。范围同样只能从本循环的槽位起点往上，不能碰外层函数的变量。
        emitCloseScope(lc.slotStart, syms.localCount);
        currentCode->emit(0x2B);                       // JMP（目标稍后回填）
        if (isBreak) lc.breakPatches.push_back(placeholderU16());
        else         lc.contPatches.push_back(placeholderU16());
    }

    // ★ P2-4：do { ... } while (cond) —— 先执行一次，continue 跳到条件判断处
    void doWhileStmt() {
        advance(); // 'do'
        LoopCtx ctx;
        ctx.handlerDepthAtLoop = handlerDepth;
        ctx.slotStart = syms.localCount;   // ★ 本循环的槽位起点（体内局部从这里往上）
        loopStack.push_back(ctx);
        size_t bodyStart = currentCode->size();
        scopedBlock();
        size_t condPos = currentCode->size();
        for (size_t p : loopStack.back().contPatches) patchU16(p, jmpOffset(p, condPos));
        expect(TK_WHILE, "while");
        expect(TK_LPAREN, "(");
        expr();
        expect(TK_RPAREN, ")");
        currentCode->emit(0x2A);                       // JZ → 结束
        size_t jzEnd = placeholderU16();
        // ★ 1.7：范围同样只能从本循环的槽位起点往上（do-while 没有自己的作用域，
        //   用 scopes.back() 会拿到外层函数的作用域 → 把外层变量也关了）
        if (!loopStack.empty())
            emitCloseScope(loopStack.back().slotStart, syms.localCount);
        currentCode->emit(0x2B);                       // 真 → 回到循环体开头
        size_t jmpBack = placeholderU16();
        size_t loopEnd = currentCode->size();
        for (size_t p : loopStack.back().breakPatches) patchU16(p, jmpOffset(p, loopEnd));
        loopStack.pop_back();
        patchU16(jzEnd, jmpOffset(jzEnd, loopEnd));
        patchU16(jmpBack, jmpOffset(jmpBack, bodyStart));
    }

    void whileStmt() {
        LoopCtx ctx;
        ctx.startPos = currentCode->size();
        loopStack.push_back(ctx);
        size_t loopStart = currentCode->size();
        advance(); // 'while'
        expect(TK_LPAREN, "(");
        expr();
        expect(TK_RPAREN, ")");
        currentCode->emit(0x2A);
        size_t jzPos = placeholderU16();
        scopedBlock();                     // ★ P0-1
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

    // ★ 错误捕获：try { ... } catch (e) { ... }
    //   PUSH_HANDLER 记录 (帧深度, 操作数栈高度, catch 落地点)；
    //   try 体正常结束 → POP_HANDLER 并跳过 catch；
    //   出错 → VM 回退到记录的状态、把【错误表】压栈、跳到 catch。
    void tryStmt() {
        advance();                                   // 'try'
        currentCode->emit(0x27);                     // PUSH_HANDLER u16(相对偏移)
        size_t landPos = placeholderU16();
        handlerDepth++;
        scopedBlock();
        handlerDepth--;
        currentCode->emit(0x28);                     // POP_HANDLER
        currentCode->emit(0x2B);
        size_t jmpEnd = placeholderU16();
        patchU16(landPos, jmpOffset(landPos, currentCode->size()));
        expect(TK_CATCH, "catch");
        expect(TK_LPAREN, "(");
        if (cur.type != TK_IDENT)
            throw runtime_error(at("catch 后需要变量名，例如 catch (e)"));
        string varName = cur.text;
        advance();
        expect(TK_RPAREN, ")");
        syms.pushScope();
        uint16_t slot = syms.allocLocal(varName);
        genStore(slot);                              // 栈顶就是错误表
        scopedBlock();
        syms.popScope();
        patchU16(jmpEnd, jmpOffset(jmpEnd, currentCode->size()));
    }

    void returnStmt() {
        if (!syms.inFunction) throw runtime_error(at("return 只能在函数内使用"));
        int retLine = cur.line;                  // ★ return 自己在第几行
        advance(); // 'return'
        uint8_t n = 0;
        // ★ 修：词法阶段会吃掉换行，于是单独一行的 `return` 会把【下一行的语句】
        //   当成返回值吞掉（`return` + 换行 + `prln("x")` 会打印 x 再返回它）。
        //   现在要求返回值必须和 return 在同一行。
        bool sameLine = (cur.line == retLine);
        if (sameLine && cur.type != TK_RBRACE && cur.type != TK_EOF && cur.type != TK_SEMI) {
            // ★ P1-5：多返回值 return a, b, c
            n = 1;
            expr();
            while (cur.type == TK_COMMA) {
                advance();
                expr();
                n++;
            }
        }
        for (int i = 0; i < handlerDepth; i++) currentCode->emit(0x28);   // ★ 注销本层 try
        currentCode->emit(0x42);
        currentCode->u8(n);      // ★ P1-10：RET n —— 明确返回 n 个值
    }

    // =========================================================================
    //  表达式（★ 判等/比较一律用同一族指令，严格类型语义由 VM 运行期决定）
    //    · 不再插入 ITOD：旧代码在比较时对【栈顶】操作数做整数→浮点转换，
    //      而栈顶是右操作数，等于转换了错的一边，导致 1 == 1.0 与
    //      1.0 == 1 结果不对称。
    //    · 编译期的 ResType 只是粗略猜测，仅用于挑选“更贴切”的指令，
    //      不再影响正确性（VM 的整数族/浮点族指令行为等价）。
    // =========================================================================
    enum ResType { RT_NULL, RT_INT, RT_DOUBLE, RT_STR, RT_BOOL, RT_TABLE, RT_FUNC };

    // 根据两个操作数的粗略静态类型，判断结果该按字符串/浮点/整数看待
    static ResType arithResult(ResType lt, ResType rt, bool isAdd) {
        if (isAdd && (lt == RT_STR || rt == RT_STR)) return RT_STR;
        if (lt == RT_DOUBLE || rt == RT_DOUBLE)      return RT_DOUBLE;
        if (lt == RT_STR || rt == RT_STR)            return RT_STR;
        return lt;
    }

    // =========================================================================
    //  ★ P1-4 / P1-7：表达式入口
    //    优先级（低 → 高）：  ?:  →  or  →  and  →  not  →  比较  →  加减 …
    //    · and / or 短路，且【返回操作数】（Lua 风格）→ `x or 默认值` 惯用法可用
    //    · not 返回真正的 bool
    //    · ?: 惰性求值，只算命中的那一支
    //    · null / false / 0 / 0.0 / "" / 空表 为假（与 if 的真值规则一致）
    // =========================================================================
    uint16_t allocTemp() {
        if (syms.inFunction) return syms.allocScratch();
        auto it = syms.globals.find("__tmp");
        if (it == syms.globals.end()) it = syms.globals.emplace("__tmp", syms.globalCount++).first;
        return it->second;
    }
    void genStore(uint16_t slot) {
        if (syms.inFunction) { currentCode->emit(0x30); currentCode->u16(slot); }
        else                 { currentCode->emit(0x04); currentCode->u16(slot); }
    }
    void genLoad(uint16_t slot) {
        if (syms.inFunction) { currentCode->emit(0x31); currentCode->u16(slot); }
        else                 { currentCode->emit(0x05); currentCode->u16(slot); }
    }

    ResType expr() { return ternary(); }

    ResType ternary() {
        ResType lt = orExpr();
        if (cur.type == TK_QUESTION) {
            advance();                                  // '?'
            currentCode->emit(0x2A);                    // JZ → 假分支
            size_t jzPos = placeholderU16();
            expr();                                     // 真分支
            currentCode->emit(0x2B);
            size_t jmpEnd = placeholderU16();
            patchU16(jzPos, jmpOffset(jzPos, currentCode->size()));
            expect(TK_COLON, ":");
            expr();                                     // 假分支
            patchU16(jmpEnd, jmpOffset(jmpEnd, currentCode->size()));
            lt = RT_INT;
        }
        return lt;
    }

    ResType orExpr() {
        ResType lt = andExpr();
        while (cur.type == TK_OR) {                     // a or b
            advance();
            uint16_t tmp = allocTemp();
            genStore(tmp);                              // [a] →
            genLoad(tmp);                               // [a]
            currentCode->emit(0x2A);                    // 真 → 结果就是 a
            size_t jzB = placeholderU16();
            genLoad(tmp);                               // [a]
            currentCode->emit(0x2B);
            size_t jmpEnd = placeholderU16();
            patchU16(jzB, jmpOffset(jzB, currentCode->size()));
            andExpr();                                  // 假 → 结果取 b
            patchU16(jmpEnd, jmpOffset(jmpEnd, currentCode->size()));
            lt = RT_INT;
        }
        return lt;
    }

    ResType andExpr() {
        ResType lt = notExpr();
        while (cur.type == TK_AND) {                    // a and b
            advance();
            uint16_t tmp = allocTemp();
            genStore(tmp);                              // [a] →
            genLoad(tmp);                               // [a]
            currentCode->emit(0x2A);                    // 假 → 结果就是 a
            size_t jzKeep = placeholderU16();
            notExpr();                                  // 真 → 结果取 b
            currentCode->emit(0x2B);
            size_t jmpEnd = placeholderU16();
            patchU16(jzKeep, jmpOffset(jzKeep, currentCode->size()));
            genLoad(tmp);                               // [a]
            patchU16(jmpEnd, jmpOffset(jmpEnd, currentCode->size()));
            lt = RT_INT;
        }
        return lt;
    }

    ResType notExpr() {
        if (cur.type == TK_NOT) {
            advance();
            notExpr();                                  // 支持 not not x
            currentCode->emit(0x1E);                    // NOT → 真 bool
            return RT_BOOL;
        }
        return compare();
    }

    // ★ P2-1：位运算（& ^ | 优先级【高于】比较，避免 C 语言里 `x & 1 == 0` 的坑）
    //   完整优先级链：?: → or → and → not → == → | → ^ → & → 移位 → 加减 → 乘除 → 一元 → 幂
    ResType bitOrExpr() {
        ResType lt = bitXorExpr();
        while (cur.type == TK_PIPE) {
            advance(); bitXorExpr();
            currentCode->emit(0x21);                    // BOR
            lt = RT_INT;
        }
        return lt;
    }
    ResType bitXorExpr() {
        ResType lt = bitAndExpr();
        while (cur.type == TK_CARET) {
            advance(); bitAndExpr();
            currentCode->emit(0x22);                    // BXOR
            lt = RT_INT;
        }
        return lt;
    }
    ResType bitAndExpr() {
        ResType lt = shiftExpr();
        while (cur.type == TK_AMP) {
            advance(); shiftExpr();
            currentCode->emit(0x20);                    // BAND
            lt = RT_INT;
        }
        return lt;
    }

    ResType compare(bool /*needDouble*/ = false) {
        ResType lt = bitOrExpr();
        if (cur.type >= TK_EQ && cur.type <= TK_GE) {
            TK op = cur.type; advance();
            bitOrExpr();
            // ★ 统一严格比较指令：类型不同 → == 为 false、!= 为 true、
            //   大小比较一律 false（含 int 与 float 之间）
            switch (op) {
                case TK_EQ:  currentCode->emit(0x10); break;
                case TK_NEQ: currentCode->emit(0x11); break;
                case TK_LT:  currentCode->emit(0x12); break;
                case TK_LE:  currentCode->emit(0x13); break;
                case TK_GT:  currentCode->emit(0x14); break;
                case TK_GE:  currentCode->emit(0x15); break;
                default: break;
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
            // 算术语义由 VM 动态分派（整数/浮点自动提升、字符串仅 + 拼接）
            if (op == TK_PLUS) {
                ResType res = arithResult(lt, rt, true);
                currentCode->emit(res == RT_INT ? 0x06 : 0x0A);
                lt = res;
            } else {
                currentCode->emit((lt == RT_DOUBLE || rt == RT_DOUBLE) ? 0x0B : 0x07);
                if (lt == RT_DOUBLE || rt == RT_DOUBLE) lt = RT_DOUBLE;
            }
        }
        return lt;
    }

    ResType muldiv() {
        ResType lt = unary();
        while (cur.type == TK_MUL || cur.type == TK_DIV || cur.type == TK_MOD) {
            TK op = cur.type; advance();
            ResType rt = unary();
            bool dbl = (lt == RT_DOUBLE || rt == RT_DOUBLE);
            if (op == TK_MOD) {
                currentCode->emit(0x0F);
                if (dbl) lt = RT_DOUBLE;
            } else if (op == TK_MUL) {
                currentCode->emit(dbl ? 0x0C : 0x08);
                if (dbl) lt = RT_DOUBLE;
            } else {
                currentCode->emit(dbl ? 0x0D : 0x09);
                if (dbl) lt = RT_DOUBLE;
            }
        }
        return lt;
    }

    // ★ P2-1：移位（比比较紧、比加减松，与 C 的算术优先级一致）
    ResType shiftExpr() {
        ResType lt = addsub();
        while (cur.type == TK_SHL || cur.type == TK_SHR) {
            bool left = (cur.type == TK_SHL);
            advance();
            addsub();
            currentCode->emit(left ? 0x23 : 0x24);      // SHL / SHR
            lt = RT_INT;
        }
        return lt;
    }

    ResType unary() {
        if (cur.type == TK_MINUS) {
            advance();
            unary();                        // 支持 --x / -x
            uint16_t tmpSlot = syms.inFunction
                ? syms.allocScratch()                       // ★ P0-1：临时槽
                : syms.allocGlobal("__neg_tmp");
            // 0 - x：SUB 由 VM 动态分派，整数/浮点都正确
            currentCode->emit(syms.inFunction ? 0x30 : 0x04); currentCode->u16(tmpSlot);
            currentCode->emit(0x01); currentCode->u16(cp.addInt(0));
            currentCode->emit(syms.inFunction ? 0x31 : 0x05); currentCode->u16(tmpSlot);
            currentCode->emit(0x07);
            return RT_INT;
        }
        if (cur.type == TK_TILDE) {          // ★ P2-1：按位取反
            advance();
            unary();
            currentCode->emit(0x1F);         // BNOT
            return RT_INT;
        }
        return power();
    }

    // ★ P2-2：幂运算（右结合，比一元负号紧：-2**2 == -4）
    ResType power() {
        ResType lt = primary();
        if (cur.type == TK_POW) {
            advance();
            unary();                         // 右结合
            currentCode->emit(0x25);         // POW
            return RT_INT;
        }
        return lt;
    }

    // ★ 任意值后面的后缀链：.字段 / [下标]
    void postfixAccess() {
        while (cur.type == TK_DOT || cur.type == TK_LBRACKET || cur.type == TK_LPAREN) {
            if (cur.type == TK_LPAREN) { emitDynamicCall(1); continue; }   // ★ 调用函数值
            if (cur.type == TK_DOT) {
                advance();
                if (cur.type != TK_IDENT) throw runtime_error(at("点号后需要字段名"));
                string f = cur.text; advance();
                currentCode->emit(0x01); currentCode->u16(cp.addString(f));
                currentCode->emit(OP_TABLE_GET);
            } else {
                advance(); expr(); expect(TK_RBRACKET, "]");
                currentCode->emit(OP_TABLE_GET);
            }
        }
    }

    // ★ 读一个变量（LOCAL_LOAD / LOAD_VAR），读路径会做"未声明"检查
    void emitLoadVar(const string& name, bool forRead) {
        VarInfo vi = syms.resolve(name);
        if (vi.kind == VarInfo::LOCAL) { currentCode->emit(0x31); currentCode->u16(vi.slot); return; }
        if (vi.kind == VarInfo::UPVAL) { currentCode->emit(0x45); currentCode->u16(vi.slot); return; }  // ★ 闭包
        if (forRead) requireDeclared(name);
        if (vi.slot == (uint16_t)-1) vi.slot = syms.allocGlobal(name);
        currentCode->emit(0x05); currentCode->u16(vi.slot);
    }
    // ★ 1.7：写变量（LOCAL / UPVAL / GLOBAL 统一入口；值在栈顶）
    void emitStoreVar(const string& name) {
        VarInfo vi = syms.resolve(name);
        if (vi.kind == VarInfo::LOCAL) { currentCode->emit(0x30); currentCode->u16(vi.slot); return; }
        if (vi.kind == VarInfo::UPVAL) { currentCode->emit(0x46); currentCode->u16(vi.slot); return; }
        if (vi.slot == (uint16_t)-1) vi.slot = syms.allocGlobal(name);
        currentCode->emit(0x04); currentCode->u16(vi.slot);
    }

    // =========================================================================
    //  ★ 1.7：闭包
    // =========================================================================
    //  编译一个闭包体（已消费到 '(' 之前）。成功返回 funcId，*nup = 上值个数。
    //  调用方负责：名字槽（递归用）与 CLOSURE 指令的发射。
    uint16_t compileClosureBody(const string& selfName, uint8_t* nup) {
        string outerName = syms.curFuncName;
        expect(TK_LPAREN, "(");
        syms.pushCtx(selfName.empty() ? std::string("<闭包>") : selfName);

        uint16_t funcId = nextUserFuncId++;
        vector<string> params;
        if (cur.type != TK_RPAREN) {
            if (cur.type != TK_IDENT) throw runtime_error(at("参数名必须是标识符"));
            params.push_back(cur.text); syms.allocParam(cur.text); advance();
            while (cur.type == TK_COMMA) {
                advance();
                if (cur.type != TK_IDENT) throw runtime_error(at("参数名必须是标识符"));
                params.push_back(cur.text); syms.allocParam(cur.text); advance();
            }
        }
        syms.funcParamCounts[funcId] = (uint16_t)params.size();
        expect(TK_RPAREN, ")");
        expect(TK_LBRACE, "{");

        CodeBuf bodyBuf;
        CodeBuf* savedCode = currentCode;
        int savedHandler = handlerDepth;
        int savedBody    = bodyDepth;
        currentCode  = &bodyBuf;
        handlerDepth = 0;                       // 函数体有自己的 try 层数
        bodyDepth    = savedBody + 1;
        bodyBuf.markLine((uint32_t)cur.line);
        while (cur.type != TK_RBRACE && cur.type != TK_EOF) stmt();
        expect(TK_RBRACE, "}");
        bodyBuf.emit(0x42);                     // 末尾补 RET 0
        bodyBuf.u8(0);

        currentCode  = savedCode;
        handlerDepth = savedHandler;
        bodyDepth    = savedBody;

        FuncBlob fb;
        fb.code       = bodyBuf.code;
        fb.lines      = bodyBuf.lines;
        fb.funcId     = funcId;
        fb.paramCount = (uint16_t)params.size();
        fb.localCount = syms.maxLocalCount;     // ★ 必须在 popCtx 之前读
        fb.name       = outerName + "." + (selfName.empty() ? std::string("<闭包>") : selfName) +
                        "#" + std::to_string(funcId);
        syms.popCtx(&fb.upvals);
        if (nup) *nup = (uint8_t)fb.upvals.size();
        funcDefs.push_back(fb);
        return funcId;
    }

    // func(x){...} —— 匿名函数表达式；也可以 func name(x){...} 让函数体能引用自己（递归）
    ResType lambdaExpr() {
        advance();                                     // 'func'
        string selfName;
        if (cur.type == TK_IDENT && peekNext().type == TK_LPAREN) { selfName = cur.text; advance(); }
        bool named = !selfName.empty();
        uint16_t selfSlot = 0;
        if (named) selfSlot = syms.allocLocal(selfName);   // 名字槽在外层作用域（体里当上值用）
        uint8_t nup = 0;
        uint16_t funcId = compileClosureBody(selfName, &nup);
        currentCode->emit(0x44);                       // CLOSURE
        currentCode->u16(funcId);
        currentCode->u8(nup);
        if (named) {
            currentCode->emit(0x30); currentCode->u16(selfSlot);   // 存进名字槽
            currentCode->emit(0x31); currentCode->u16(selfSlot);   // 读出来当表达式结果
        }
        return RT_FUNC;
    }

    // 栈顶已是函数值：吃 '(' 参数 ')' 并发 CALL_VALUE
    void emitDynamicCall(uint8_t want = 1) {
        advance();                                     // '('
        uint8_t argc = 0;
        if (cur.type != TK_RPAREN) {
            argc = 1; expr();
            while (cur.type == TK_COMMA) { advance(); expr(); argc++; }
        }
        expect(TK_RPAREN, ")");
        currentCode->emit(0x48);                       // CALL_VALUE
        currentCode->u8(argc);
        // ★ 约定：lastCallWantPos 是【want 字节本身的偏移】（多返回值机制会直接改这个字节），
        //   所以必须在 emit(want) 之前记，否则补丁会打到指令后面、把后续字节写坏。
        lastCallWantPos = currentCode->size();
        currentCode->u8(want);
        lastCallEndSize = currentCode->size();         // 让多返回值机制能认出"整项就是一个调用"
    }

    ResType primary() {
        if (cur.type == TK_NUM) {
            if (cur.isFloat) {
                currentCode->emit(0x01); currentCode->u16(cp.addDouble(cur.num)); advance();
                return RT_DOUBLE;
            } else {
                // ★ P0-2：按 int64 入池（小整数仍用 INT32 tag）
                currentCode->emit(0x01); currentCode->u16(cp.addIntAuto(cur.ival)); advance();
                return RT_INT;
            }
        } else if (cur.type == TK_STR) {
            currentCode->emit(0x01); currentCode->u16(cp.addString(cur.text)); advance();
            return RT_STR;
        } else if (cur.type == TK_BOOL) {
            // ★ 真 bool 常量：不再用整数 1/0 冒充（否则 type() 分不出、
            //   且 true == 1 会成立，违反严格类型判等）
            currentCode->emit(cur.num != 0 ? 0x1C : 0x1D);
            advance();
            return RT_BOOL;
        } else if (cur.type == TK_NULL) {                    // ★ null 字面量
            currentCode->emit(0x01); currentCode->u16(cp.addNull()); advance();
            return RT_NULL;
        } else if (cur.type == TK_FUNC) {                    // ★ 1.7：闭包表达式

            ResType r = lambdaExpr();
            postfixAccess();                                 // 允许 func(){...}() 之类
            return r;
        } else if (cur.type == TK_LBRACE) {
            tableLiteral();
            return RT_TABLE;
        } else if (cur.type == TK_LPAREN) {
            advance();
            ResType d = expr();
            expect(TK_RPAREN, ")");
            postfixAccess();          // ★ 1.7：(f)(x) / (func(){...})()
            return d;
        } else if (cur.type == TK_IDENT) {
            string name = cur.text;
            Token peek = peekNext();
            if (peek.type == TK_LPAREN) {
                uint8_t bid = builtinId(name);
                if (bid != 0xFF) {
                    callBuiltin(bid, 1);
                    postfixAccess();                          // ★ 支持 f(...).field / f(...)[k]
                    if (bid == BID_str)   return RT_STR;
                    if (bid == BID_float) return RT_DOUBLE;
                    if (bid == BID_type)  return RT_STR;      // ★ type() 返回字符串
                    return RT_INT;
                }
                // ★ 原生库裸调用（用户函数优先）
                if (!localFuncExists(name) && !impFuncs.count(name)) {
                    auto nit = natives.find(name);
                    if (nit != natives.end()) {
                        int nLine = cur.line, nCol = cur.col;   // ★ 记下函数名位置（诊断用）
                        advance();                           // 越过函数名，cur 落到 '('
                        emitNativeCall(name, nit->second, 1, nLine, nCol);
                        postfixAccess();
                        return RT_INT;                        // 类型运行期决定
                    }
                }
                // ★ 1.7：不是已知函数，但能解析成变量 → 那是装着闭包的变量，动态调用
                if (!localFuncExists(name) && !impFuncs.count(name)) {
                    VarInfo vi = syms.resolve(name);
                    bool callableVar = (vi.kind == VarInfo::LOCAL) || (vi.kind == VarInfo::UPVAL) ||
                                       (vi.kind == VarInfo::GLOBAL && vi.slot != (uint16_t)-1);
                    if (callableVar) {
                        advance();                    // 越过变量名，cur 落到 '('
                        emitLoadVar(name, true);      // 压入函数值
                        emitDynamicCall(1);
                        postfixAccess();
                        return RT_INT;
                    }
                }
                callUserFunc(1);
                postfixAccess();
                return RT_INT;
            }
            if (peek.type == TK_DOT) {
                // ★ 点号有两种用法：限定库调用 io.readFile(...)  或  表字段 e.code
                advance();                                  // 越过基名
                expect(TK_DOT, ".");
                if (cur.type != TK_IDENT)
                    throw runtime_error(at("点号后需要字段名或库函数名"));
                string fld  = cur.text;
                int fLine = cur.line, fCol = cur.col;
                advance();                                  // 越过字段名
                string key = name + "." + fld;
                if (isModuleFunction(name, fld)) {
                    if (cur.type != TK_LPAREN)
                        throw runtime_error(atPos(fLine, fCol, (int)fld.size(),
                            "库函数 '" + key + "' 必须加括号调用"));
                    emitQualifiedCall(name, fld, 1, fLine, fCol);
                    postfixAccess();
                    return RT_INT;
                }
                if (moduleImported(name) && cur.type == TK_LPAREN)
                    throw runtime_error(at("库 '" + name + "' 里没有函数 '" + fld + "'"));
                // 表字段访问：t.name 等价 t["name"]
                emitLoadVar(name, true);
                currentCode->emit(0x01); currentCode->u16(cp.addString(fld));
                currentCode->emit(OP_TABLE_GET);
                postfixAccess();          // ★ 继续吃后缀链：.字段 / [下标] / 调用函数值
                return RT_TABLE;
            }
            if (peek.type == TK_LBRACKET) {
                // 读取：name [key1] [key2] ... —— 链式子索引（表始终在栈顶）
                emitLoadVar(name, true);              // ★ 闭包：读变量（含上值）
                advance(); // name
                while (cur.type == TK_LBRACKET) {
                    advance(); // '['
                    expr();   // key 表达式 → 栈
                    expect(TK_RBRACKET, "]");
                    currentCode->emit(OP_TABLE_GET);
                }
                postfixAccess();          // ★ 支持 t[i].field / t[i][j].field
                return RT_TABLE;
            }
            {
                emitLoadVar(name, true);              // ★ 闭包：读变量（含上值）
                advance();
                return RT_INT;
            }
        } else {
            throw runtime_error(at("语法错误: 无法解析 '" + cur.text + "'", (int)cur.text.size()));
        }
    }

    bool builtinHasReturn(uint8_t bid) const {
        return (bid != BID_pr && bid != BID_prln);
    }

    // ★ P1-5：want = 调用方希望得到几个返回值
    //   0 = 全部丢弃（语句上下文）  1 = 只要第一个（表达式上下文）
    //   k = 恰好 k 个（k 元赋值/返回的最后一个位置）
    void callBuiltin(uint8_t bid, uint8_t want = 1) {
        advance(); // func name
        expect(TK_LPAREN, "(");
        uint8_t argc = 0;
        if (cur.type != TK_RPAREN) {
            argc = 1;
            expr();
            while (cur.type == TK_COMMA) {
                advance();
                expr();
                argc++;
            }
        }
        expect(TK_RPAREN, ")");
        currentCode->emit(0x02);
        currentCode->u8(bid);
        currentCode->u8(argc);
        lastCallWantPos = currentCode->size();      // ★ P1-5：记下 want 字节位置
        currentCode->u8(want);
        lastCallEndSize = currentCode->size();
    }

    // 这个名字在当前上下文里是不是"本地函数"（主文件函数 / 本模块自己的函数）
    bool localFuncExists(const string& name) const {
        if (!curModule.empty())
            return syms.functions.count(curModule + "." + name) != 0;
        return syms.functions.count(name) != 0;
    }

    void callUserFunc(uint8_t want = 1) {
        string funcName = cur.text;
        int    nameLine = cur.line, nameCol = cur.col;      // ★ 记下函数名位置
        advance();
        // ★ 1.7：既不是本地函数、也不是导入模块的函数/原生函数，但能解析成【变量】——
        //   那就是一个装着闭包的变量，走动态调用（语句位置也走这里，所以放在这个公共出口）。
        //   注意：必须在吃掉 '(' 之前把函数值压栈，栈序是 [函数值, 参数...]。
        {
            string key = curModule.empty() ? funcName : (curModule + "." + funcName);
            if (!syms.functions.count(key) && !impFuncs.count(funcName) && !natives.count(funcName)) {
                VarInfo vi = syms.resolve(funcName);
                bool callableVar = (vi.kind == VarInfo::LOCAL) || (vi.kind == VarInfo::UPVAL) ||
                                   (vi.kind == VarInfo::GLOBAL && vi.slot != (uint16_t)-1);
                if (callableVar) {
                    emitLoadVar(funcName, true);
                    emitDynamicCall(want);
                    return;
                }
            }
        }
        expect(TK_LPAREN, "(");
        auto it = syms.functions.end();
        if (!curModule.empty()) it = syms.functions.find(curModule + "." + funcName);
        if (it == syms.functions.end() && curModule.empty()) it = syms.functions.find(funcName);
        if (it == syms.functions.end()) {
            // 导入的 .m 模块导出的函数
            auto mit = impFuncs.find(funcName);
            if (mit != impFuncs.end()) {
                uint8_t argc = 0;
                if (cur.type != TK_RPAREN) { argc = 1; expr(); while (cur.type == TK_COMMA) { advance(); expr(); argc++; } }
                expect(TK_RPAREN, ")");
                if ((uint16_t)argc != mit->second.params)
                    throw runtime_error(atPos(nameLine, nameCol, (int)funcName.size(),
                        "模块函数 " + funcName + " 需要 " + to_string(mit->second.params) +
                        " 个参数，实际给了 " + to_string(argc)));
                currentCode->emit(0x41); currentCode->u16(mit->second.funcId);
                currentCode->u8(argc); currentCode->u8(want);
                return;
            }
            throw runtime_error(atPos(nameLine, nameCol, (int)funcName.size(),
                "未定义的函数: " + funcName + "（若它来自原生库，请先在文件里写 imp <库名>）"));
        }
        uint16_t funcId = it->second;
        uint16_t expectedArgs = syms.funcParamCounts[funcId];
        uint8_t argc = 0;
        if (cur.type != TK_RPAREN) {
            argc = 1;
            expr();
            while (cur.type == TK_COMMA) { advance(); expr(); argc++; }
        }
        expect(TK_RPAREN, ")");
        if (argc != (uint8_t)expectedArgs) {
            throw runtime_error(atPos(nameLine, nameCol, (int)funcName.size(),
                "函数 '" + funcName + "' 参数数量不匹配: 期望 " +
                to_string(expectedArgs) + "，得到 " + to_string(argc)));
        }
        currentCode->emit(0x41);
        currentCode->u16(funcId);
        currentCode->u8(argc);
        lastCallWantPos = currentCode->size();      // ★ P1-5
        currentCode->u8(want);
        lastCallEndSize = currentCode->size();
    }

    // ★ 原生库调用（cur 必须指向 '('）
    //   nameLine/nameCol = 函数名 token 的位置（-1 = 调用方没传，退回到 '(' 的位置）
    //   ★ 修：参数个数错误原来用 at()，那会儿已经吃掉了 ')'，光标会指到【下一条语句】
    void emitNativeCall(const string& key, const NativeInfo& info, uint8_t want,
                        int nameLine = -1, int nameCol = -1) {
        const int lpLine = cur.line, lpCol = cur.col;      // cur 此刻指向 '('
        auto atCall = [&](const string& msg) {
            if (nameLine >= 0) return atPos(nameLine, nameCol, (int)key.size(), msg);
            return atPos(lpLine, lpCol, 1, msg);
        };
        expect(TK_LPAREN, "(");
        uint8_t argc = 0;
        if (cur.type != TK_RPAREN) {
            argc = 1;
            expr();
            while (cur.type == TK_COMMA) { advance(); expr(); argc++; }
        }
        expect(TK_RPAREN, ")");
        if (info.minArgs >= 0 && (int)argc < info.minArgs)
            throw runtime_error(atCall("库函数 " + key + " 至少需要 " + to_string(info.minArgs) +
                                       " 个参数，实际给了 " + to_string(argc)));
        if (info.maxArgs >= 0 && (int)argc > info.maxArgs)
            throw runtime_error(atCall("库函数 " + key + " 最多接受 " + to_string(info.maxArgs) +
                                       " 个参数，实际给了 " + to_string(argc)));
        currentCode->emit(0x26);                     // NATIVE_NAME u16(常量) u8(argc) u8(want)
        currentCode->u16(cp.addString(key));
        currentCode->u8(argc);
        currentCode->u8(want);
    }

    // 这个限定名是不是某个模块/原生库的函数
    bool isModuleFunction(const string& modName, const string& fname) const {
        if (natives.count(modName + "." + fname)) return true;
        auto mi = modules.find(modName);
        return mi != modules.end() && mi->second.count(fname) != 0;
    }
    // ★ 限定调用 mod.func(...)：原生库走 NATIVE，Ae 模块走 CALL_FUNC
    void emitQualifiedCall(const string& modName, const string& fname, uint8_t want,
                           int nameLine = -1, int nameCol = -1) {
        string key = modName + "." + fname;
        if (natives.count(key)) { emitNativeCall(key, natives[key], want, nameLine, nameCol); return; }
        auto mi = modules.find(modName);
        if (mi != modules.end()) {
            auto fi = mi->second.find(fname);
            if (fi != mi->second.end()) {
                ModFunc mf = fi->second;
                expect(TK_LPAREN, "(");
                uint8_t argc = 0;
                if (cur.type != TK_RPAREN) { argc = 1; expr(); while (cur.type == TK_COMMA) { advance(); expr(); argc++; } }
                expect(TK_RPAREN, ")");
                if ((uint16_t)argc != mf.params)
                    throw runtime_error(at("模块函数 " + key + " 需要 " + to_string(mf.params) +
                                           " 个参数，实际给了 " + to_string(argc)));
                currentCode->emit(0x41); currentCode->u16(mf.funcId);
                currentCode->u8(argc); currentCode->u8(want);
                return;
            }
            throw runtime_error(at("模块 '" + modName + "' 里没有导出的函数 '" + fname + "'"));
        }
        if (!moduleImported(modName))
            throw runtime_error(at("未导入的库 '" + modName + "'：库函数请先写 imp " + modName +
                                   "；表字段请写成 " + modName + "[\"" + fname + "\"]"));
        throw runtime_error(at("库 '" + modName + "' 里没有函数 '" + fname + "'"));
    }

    void callExpr(uint8_t want = 1) {
        string name = cur.text;
        // ★ 限定调用 mod.func(...)
        if (peekNext().type == TK_DOT) {
            Token fld = peekAhead(1);
            advance();                                   // mod
            expect(TK_DOT, ".");
            if (cur.type != TK_IDENT)
                throw runtime_error(at("点号后需要库函数名"));
            advance();                                   // func —— 之后 cur 才是 '('
            emitQualifiedCall(name, fld.text, want, fld.line, fld.col);
            return;
        }
        uint8_t bid = builtinId(name);
        if (bid != 0xFF) { callBuiltin(bid, want); return; }
        // 用户函数优先于同名原生函数
        if (!localFuncExists(name) && !impFuncs.count(name)) {
            auto it = natives.find(name);
            if (it != natives.end()) {
                int nLine = cur.line, nCol = cur.col;    // ★ 记下函数名位置（诊断用）
                advance();                           // 越过函数名，cur 落到 '('
                emitNativeCall(name, it->second, want, nLine, nCol);
                return;
            }
        }
        callUserFunc(want);
    }

    void compile() { program(); }

    void save(const string& path) {
        vector<uint8_t> out;
        out.insert(out.end(), {'A', 'e', 'B', 'c'});
        out.push_back(1);   // major
        out.push_back(7);   // ★ minor = 7：闭包（FUNC_DEF 头带上值描述表 + CLOSURE 系列指令）
        out.push_back(0);
        out.push_back(0);   // flags
        cp.write(out);

        // ① 源文件名（供运行期诊断显示）
        writeU32(out, (uint32_t)srcName.size());
        out.insert(out.end(), srcName.begin(), srcName.end());

        // ①b 源文件表（.m 模块会让程序涉及多个源文件）
        writeU32(out, (uint32_t)fileNames.size());
        for (const string& fn : fileNames) {
            writeU32(out, (uint32_t)fn.size());
            out.insert(out.end(), fn.begin(), fn.end());
        }

        // ② 函数名表（供调用栈回溯）
        writeU32(out, (uint32_t)funcNames.size());
        for (const auto& fn : funcNames) {
            writeU16(out, fn.first);
            writeU32(out, (uint32_t)fn.second.size());
            out.insert(out.end(), fn.second.begin(), fn.second.end());
        }

        // ③ 行号表（指令偏移 → 源码行，按偏移升序）
        writeU32(out, (uint32_t)allLines.size());
        for (const auto& L : allLines) {
            writeU32(out, L.off);
            writeU32(out, L.line);
            writeU32(out, L.col);
            writeU16(out, L.file);
        }

        output.write(out);

        // ★ 1.3：导入的原生库列表（放在代码段之后 —— 旧 loader 读到 codeLen 就停，天然兼容）
        writeU32(out, (uint32_t)importedModules.size());
        for (const string& m : importedModules) {
            writeU32(out, (uint32_t)m.size());
            out.insert(out.end(), m.begin(), m.end());
        }
        ofstream f(path, ios::binary);
        f.write((const char*)out.data(), static_cast<streamsize>(out.size()));
    }
};

// =============================================================================
//  ★ 诊断渲染：把结构化消息变成专业报错
//    分类表集中在这里维护 —— 新增错误类型只改这一张表
// =============================================================================
static const char* classifyDiag(const std::string& m) {
    struct Rule { const char* frag; const char* code; };
    static const Rule kRules[] = {
        { "语法错误: 期望",     AE_E_SYNTAX },
        { "语法错误: 无法解析", AE_E_SYNTAX },
        { "非法字符",           AE_E_BADCHAR },
        { "字符串字面量未闭合", AE_E_STR_UNCLOSED },
        { "块注释",             AE_E_CMT_UNCLOSED },
        { "未定义的函数",       AE_E_UNDEF_FUNC },
        { "未声明的变量",       AE_E_UNDECLARED },
        { "变量 '",             AE_E_UNDEF_VAR },
        { "重复定义",           AE_E_DUP_FUNC },
        { "是内建函数名",       AE_E_SHADOW_BUILTIN },
        { "参数数量不匹配",     AE_E_ARITY },
        { "至少需要",           AE_E_LIB_ARITY },
        { "最多接受",           AE_E_LIB_ARITY },
        { "里没有函数",         AE_E_LIB_NOFUNC },
        { "未导入的库",         AE_E_NO_IMPORT },
        { "找不到库",           AE_E_LIB_NOTFOUND },
        { "没有导出任何函数",   AE_E_LIB_NOTFOUND },
        { "ABI 版本",           AE_E_LIB_ABI },
        { "常量超出",           AE_E_NUM_RANGE },
        { "字面量缺少",         AE_E_NUM_RANGE },
        { "不是合法 UTF-8",     AE_E_BAD_UTF8 },
        { "break",              AE_E_LOOP_CTRL },
        { "continue",           AE_E_LOOP_CTRL },
        { "只能在函数内使用",   AE_E_SCOPE },
        { "多于左侧",           AE_E_MULTI_ASSIGN },
        { "表键",               AE_E_TABLE_LIT },
        { "main 不能有参数",    AE_E_MAIN_SIG },
    };
    for (const Rule& r : kRules)
        if (m.find(r.frag) != std::string::npos) return r.code;
    return AE_E_SYNTAX;
}
static std::string helpForDiag(const std::string& m) {
    struct Rule { const char* frag; const char* help; };
    static const Rule kRules[] = {
        { "未声明的变量", "赋值可以隐式创建全局变量；函数内请先用 local 声明。" },
        { "未定义的函数", "若这个函数来自原生库，请在文件里写 imp <库名>。" },
        { "未导入的库",   "库函数要先写 imp <库名>；表字段请写成 t[\"key\"]。" },
        { "找不到库",     "库文件应放在 <工具链目录>/lib/ 下，或用环境变量 AE_LIB_PATH 指定目录。" },
        { "参数数量不匹配", "用户函数的参数个数必须完全一致，没有默认值。" },
        { "不是合法 UTF-8", "请把源文件另存为 UTF-8（无 BOM 也可以）。" },
        { "多于左侧",     "多赋值时右侧表达式个数不能多于左侧目标个数。" },
    };
    for (const Rule& r : kRules)
        if (m.find(r.frag) != std::string::npos) return r.help;
    return std::string();
}
static std::string readWholeFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::string();
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
static void renderDiag(const std::string& what, const std::string& file,
                       const std::string& src, const std::vector<std::string>* files = nullptr) {
    int fi = 0, line = 0, col = 1, len = 1;
    std::string msg = what;
    if (aeUnpack(what, fi, line, col, len, msg)) {
        std::string useFile = file;
        std::string useSrc  = src;
        if (files && fi > 0 && (size_t)fi < files->size()) {
            useFile = (*files)[(size_t)fi];                 // ★ 报错来自 .m 模块
            useSrc  = readWholeFile(useFile);
        }
        aediag::AeDiag d;
        d.code  = classifyDiag(msg);
        d.msg   = msg;
        d.span  = { useFile, line, col, len };
        d.hasSpan = true;
        std::string h = helpForDiag(msg);
        if (!h.empty()) d.helps.push_back(h);
        aediag::printDiag(d, useSrc);
    } else {
        // 兼容尚未改成结构化格式的抛出点：尽量从 "第 N 行 M 列: 消息" 里恢复位置
        std::string m2 = what;
        int l2 = 0, c2 = 1;
        if (what.rfind("\xe7\xac\xac ", 0) == 0) {               // 以 "第 " 开头
            size_t a = what.find(" \xe8\xa1\x8c ");               // " 行 "
            size_t b = what.find(" \xe5\x88\x97: ");              // " 列: "
            if (a != std::string::npos && b != std::string::npos) {
                l2 = atoi(what.substr(4, a - 4).c_str());
                c2 = atoi(what.substr(a + 5, b - a - 5).c_str());
                m2 = what.substr(b + 6);
            }
        }
        aediag::AeDiag d;
        d.code = classifyDiag(m2);
        d.msg  = m2;
        if (l2 > 0) { d.span = { file, l2, c2, 1 }; d.hasSpan = true; }
        std::string h = helpForDiag(m2);
        if (!h.empty()) d.helps.push_back(h);
        aediag::printDiag(d, src);
    }
}

// =============================================================================
//  main
// =============================================================================
int main(int argc, char** argv) {
    aecon::install();    // ★ 第一件事：把 cout/cerr/cin 接到控制台（GBK 的 cmd 里中文不乱码）
    string full;            // 源码文本（诊断时要摘录出错那一行）
    vector<string> c_files; // 编译期的源文件表（含 .m 模块）
    std::unique_ptr<Compiler> c;      // 提到 try 外：抛错后仍能拿到文件表
    string inPath, outPath;
    bool quiet = false;   // -q：编程式调用时不打印成功提示
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            string a = argv[i];
            if (a == "-h" || a == "--help") {          // ★ 加：以前 aec -h 会被当成源文件名
                cout << "Ae 编译器 (aec) 1.0\n"
                        "用法: aec <源文件.ae> [-o 输出.aeo] [-q]\n"
                        "  -o <文件>        指定输出的字节码文件名（默认同名 .aeo）\n"
                        "  -q, --quiet      成功时不打印提示（只出错才输出）\n"
                        "  -h, --help       显示本帮助\n"
                        "退出码: 0 成功  1 编译失败\n"
                        "提示: 用法里第二个位置参数也可以直接写输出文件（aec a.ae b.aeo）\n";
                return 0;
            }
            if (a == "-q" || a == "--quiet") {
                quiet = true;
            } else if (a == "-o" && i + 1 < argc) {
                outPath = argv[++i];
            } else if (!inPath.empty() && outPath.empty() && a[0] != '-') {
                outPath = a;
            } else if (inPath.empty()) {
                inPath = a;
            }
        }
    } else {
        cout << "Ae 编译器 (aec) - 输入源文件路径: ";
        getline(cin, inPath);
        if (inPath.empty()) { aediag::printBrief(AE_E_USAGE, "未提供输入文件（aec -h 看用法）"); return 1; }
    }
    if (inPath.empty()) { aediag::printBrief(AE_E_USAGE, "未指定输入文件"); return 1; }
    if (inPath.size() > 2 && inPath.substr(inPath.size() - 2) == ".m") {
        aediag::printBrief(AE_E_USAGE, ".m 是模块文件（由 imp 使用），不能直接编译；程序入口请用 .ae");
        return 1;
    }

    try {
        ifstream srcf(inPath);
        if (!srcf) { aediag::printBrief(AE_E_FILE, "无法打开源文件: " + inPath); return 1; }
        stringstream ss; ss << srcf.rdbuf();
        full = ss.str();

        c = std::make_unique<Compiler>(full, inPath);
        c->compile();
        c_files = c->fileNames;
        if (outPath.empty()) {
            outPath = inPath;
            if (outPath.size() > 3 && outPath.substr(outPath.size() - 3) == ".ae")
                outPath = outPath.substr(0, outPath.size() - 3) + ".aeo";
            else outPath += ".aeo";
        }
        c->save(outPath);
    } catch (const exception& e) {
        renderDiag(e.what(), inPath, full, c ? &c->fileNames : nullptr);
        return 1;
    }
    c_files.clear();
    if (!quiet) {
        ifstream chk(outPath, ios::binary | ios::ate);
        long long sz = chk ? (long long)chk.tellg() : 0;
        aediag::printOk("编译成功: " + outPath + "（" + to_string(sz) + " 字节）");
    }
    return 0;
}