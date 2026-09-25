// =============================================================================
//  Ae 虚拟机 (ae = Ae VM)  ——  支持自定义函数 + 局部变量 + 递归
//                            + 字符串拼接 + len/str/int/float 内置函数
//                            + 【数组】1-based 索引，引用语义（Lua 表语义）
//                            + ★ 值的基石：null + 严格类型判等 + 可哈希 + type()
// -----------------------------------------------------------------------------
//  ▌值的基石（Value Foundation）设计约定：
//     ① 类型标签 ValueKind：VAL_NULL 表示“无值”，仅与自身相等。
//     ② 判等（== / !=）采用【严格类型 + 值】语义：
//          - 类型不同 → 永远 false（含 1 != 1.0、null != false）
//          - 整数与浮点也视为不同类型（不隐式转换），符合“可预测”
//     ③ 哈希（hashOf）仅对标量：INT / DOUBLE / STR / BOOL / NULL 可哈希；
//        表不可哈希（作为表键会运行时报错），为下一步通用表预留。
//     ④ type(v) 内置函数 → 返回类型名字符串，与 ValueKind 一一对应。
//     ⑤ 表（TABLE）的 == / != 按【引用身份】比较（同一个表对象才相等）。
//
//  ▌★ 本轮修复（P0 批次：语义错误与未定义行为）：
//     · P0-2 常量池支持 INT64（tag=5）：运行期是 int64，字面量此前却按 int32 存，
//       `prln(3000000000)` 会抛 C++ 原始异常 "stoi argument out of range"。
//     · P0-3 判等与大小比较分开定案：
//         == / !=   —— 与 null 比较永远合法；int 与 float 之间报类型错；
//                      其余类型不同 → false / true
//         < <= > >= —— 类型不同一律报类型错；null / 表没有大小关系
//       （旧实现在跨类型时静默返回 false：`f = 1.5` 时 `f > 1` 得 false，
//         而同一份代码里 `f + 1` 却能自动提升成 2.5，静默给错答案。）
//     · P0-4 bool 不再是数值：`true + 1` 报类型错（它有自己的 type()，
//       且 `true == 1` 为 false，允许相加会自相矛盾）。显式 int(true) 仍然可用。
//     · P0-6 整数溢出定义为二进制回绕（用无符号中转，消除有符号溢出 UB，
//       并显式定义 INT64_MIN / -1 这两个 C++ 未定义运算的结果）。
//     · P0-7 int()/float() 转换失败抛运行时错误，不再静默返回 0。
//
//  ▌已知约定（P0-5）：
//     · 表的 == / != 按【引用身份】比较。
//     · 打印表时做环检测：自引用/环形引用输出 <cycle>，超过 64 层输出 <deep>，
//       不会崩溃。但引用环本身【不会被回收】（shared_ptr 环），
//       当前约定由脚本作者负责避免构造环；彻底解决需要 mark-sweep GC。
// =============================================================================

#ifdef _WIN32
  #define _CRT_SECURE_NO_WARNINGS
#endif

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <memory>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <functional>   // hash
#include <cstddef>
#include <map>
#include <cmath>        // fmod
#include <algorithm>    // reverse / sort
#include <set>
#include <cstdio>
#include <cstdlib>
#include "ae_libhost.h"      // ★ 原生库加载 + ABI
#include "ae_diag.h"         // ★ 诊断系统
#include "ae_console.h"     // ★ 控制台中文：绕开代码页

// ★ 原生库 API 表（定义在文件后部，这里先声明）
class VM;
static const AeApi* aeNativeApi();
// ★ 运行期错误分类（定义在文件后部；VM 构造错误表要用）
static const char* classifyRun(const std::string& m);
static const char* kindForCode(const char* code);
static std::string helpForRun(const std::string& m);
static std::string g_nativeLastError;      // set_error 的落脚点，供 lastError() 读取
#define ABI_VERSION_EXPECTED AE_ABI_VERSION

using namespace std;

// 字节码读取辅助（大端）
static uint8_t  readU8 (const vector<uint8_t>& b, size_t& p) { return b[p++]; }
static uint16_t readU16(const vector<uint8_t>& b, size_t& p) {
    uint16_t v = (uint16_t)((b[p] << 8) | b[p+1]); p += 2; return v;
}
static uint32_t readU32(const vector<uint8_t>& b, size_t& p) {
    uint32_t v = (uint32_t)((b[p] << 24) | (b[p+1] << 16) | (b[p+2] << 8) | b[p+3]); p += 4; return v;
}

// 常量池条目
struct Const {
    enum { UTF8 = 1, INT32 = 2, DOUBLE = 3, NULLV = 4, INT64 = 5 } tag;   // ★ 与编译器一致
    string   s;
    int32_t  i = 0;
    int64_t  i64 = 0;    // ★ P0-2
    double   d = 0;
};

// =============================================================================
//  运行时值类型标签
//   ★ 通用表（table）：数组 / 字典合二为一
//     - 连续整数键（1-based）存在数组段 `arr`，实现 O(1) 随机访问与 #t
//     - 其余所有键（int / string / bool）存在哈希段 `map`
//     - 赋值时按键自动分流：整数键 → 数组段，其他 → 哈希段
//     - 读取未定义键 → 返回 null（与 Lua 一致）
// =============================================================================
enum ValueKind {
    VAL_NULL  = 0,
    VAL_INT   = 1,
    VAL_DOUBLE = 2,
    VAL_STR   = 3,
    VAL_BOOL  = 4,
    VAL_TABLE = 5,  // ★ 原 VAL_ARR 位置升级为“通用表”
    VAL_FUNC  = 6   // ★ 闭包（函数值）
};

struct Value;    // 前置声明
struct Closure;  // 前置声明（闭包，见 Value 之后）

// 哈希段：键（严格判等 + 可哈希） → 值
struct TableKey {
    ValueKind kind;
    int64_t   i;
    double    f;
    shared_ptr<string> str;

    bool equals(const TableKey& o) const;
    size_t hash() const;

    bool operator<(const TableKey& o) const {
        if (kind != o.kind) return kind < o.kind;
        switch (kind) {
            case VAL_INT:
            case VAL_BOOL:  return i < o.i;
            case VAL_DOUBLE: return f < o.f;
            case VAL_STR: {
                const char* sa = str ? str->c_str() : "";
                const char* sb = o.str ? o.str->c_str() : "";
                return strcmp(sa, sb) < 0;
            }
            default: return false;
        }
    }
};

struct Table {
    // 数组段：逻辑索引 1 → 内部下标 0，尾部自动扩容（Lua 风格）
    vector<Value> arr;

    // 哈希段
    std::map<TableKey, Value> mapv;

    bool get(const Value& key, Value& out) const;
    void set(const Value& key, const Value& val);
    size_t length() const;      // #t：连续整数键前缀长度
};

struct Value {
    ValueKind kind;
    union {
        int64_t  i;
        double   f;
    };
    shared_ptr<string>  str;    // 仅 VAL_STR
    shared_ptr<Table>   tab;    // 仅 VAL_TABLE
    shared_ptr<Closure> fn;     // ★ 仅 VAL_FUNC

    Value() : kind(VAL_NULL) {}
    Value(int64_t v) : kind(VAL_INT), i(v) {}
    Value(double v)  : kind(VAL_DOUBLE), f(v) {}
    explicit Value(shared_ptr<string> s) : kind(VAL_STR), str(s) {}
    Value(bool b)    : kind(VAL_BOOL), i(b ? 1 : 0) {}
    Value(const string& s) : kind(VAL_STR), str(make_shared<string>(s)) {}

    static Value makeNull()  { return Value(); }
    static Value makeTable(const shared_ptr<Table>& t) {
        Value v; v.kind = VAL_TABLE; v.tab = t; return v;
    }

    bool isTable() const { return kind == VAL_TABLE; }
    bool isFunc()  const { return kind == VAL_FUNC; }
    bool isNull()  const { return kind == VAL_NULL; }

    static Value makeFunc(const shared_ptr<Closure>& c) {
        Value v; v.kind = VAL_FUNC; v.fn = c; return v;
    }
};

// =============================================================================
//  ★ 闭包：上值 + 函数值
// -----------------------------------------------------------------------------
//  Upvalue = 被闭包捕获的"外部变量"。open 非空时它直接指向某个调用帧的
//  locals[slot] —— 所以闭包和原作用域【共享同一个变量】(引用捕获)，两边改都生效。
//  帧退出时 close() 把值搬进 closed，之后闭包独立持有它（这就是闭包能"活过"
//  创建它的函数的原因，也是必须 close 的原因：不 close 就会指向已释放的帧）。
// =============================================================================
struct Upvalue {
    Value* open = nullptr;       // 指向某帧 locals[slot]；nullptr = 已关闭
    Value  closed;               // 关闭后的副本

    void close() { if (open) { closed = *open; open = nullptr; } }
    Value& get() { return open ? *open : closed; }
};

// 闭包 = 函数原型（funcId）+ 捕获到的上值表
struct Closure {
    uint16_t funcId = 0;
    std::vector<std::shared_ptr<Upvalue>> ups;
};

// =============================================================================
//  ★ 值的基石：严格判等 / 哈希 / 类型名
// =============================================================================

// =============================================================================
//  TableKey：哈希段的键
//   - 允许类型：int / string / bool（float 也允许，但建议脚本层避免）
//   - 严格判等（类型不同即不等），与 valueEquals 一致
// =============================================================================
inline bool TableKey::equals(const TableKey& o) const {
    if (kind != o.kind) return false;
    switch (kind) {
        case VAL_INT:   return i == o.i;
        case VAL_BOOL:  return i == o.i;
        case VAL_DOUBLE: return f == o.f;
        case VAL_STR: {
            const char* sa = str ? str->c_str() : "";
            const char* sb = o.str ? o.str->c_str() : "";
            return strcmp(sa, sb) == 0;
        }
        default:        return false;   // null 不允许作键
    }
}

inline size_t TableKey::hash() const {
    size_t h = (size_t)kind;
    switch (kind) {
        case VAL_INT:   return h ^ (size_t)(i + 0x9E3779B97F4A7C15LL);
        case VAL_BOOL:  return h ^ (size_t)(i ? 1 : 0);
        case VAL_DOUBLE: {
            uint64_t bits; memcpy(&bits, &f, 8);
            return h ^ (size_t)(bits ^ (bits >> 32));
        }
        case VAL_STR: {
            const char* s = str ? str->c_str() : "";
            std::hash<string> hs;
            return h ^ hs(std::string(s));
        }
        default:        return h;
    }
}

// Value → TableKey（仅限允许的键类型）
inline TableKey toKey(const Value& v) {
    TableKey k;
    k.kind = v.kind;
    switch (v.kind) {
        case VAL_INT:   k.i = v.i; break;
        case VAL_BOOL:  k.i = v.i; break;
        case VAL_STR:   k.str = v.str; break;
        case VAL_DOUBLE:                       // ★ P2-17：浮点键定案为错误
            throw runtime_error("表键不能是 float（请用 int(x) 或 str(x) 明确键类型）");
        default:
            throw runtime_error("该类型不可用作表键（仅允许 int / string / bool）");
    }
    return k;
}

// 严格判等：类型不同即 false，无任何隐式转换
inline bool valueEquals(const Value& a, const Value& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case VAL_NULL:  return true;
        case VAL_INT:   return a.i == b.i;
        case VAL_DOUBLE: return a.f == b.f;
        case VAL_BOOL:  return a.i == b.i;
        case VAL_STR: {
            const char* sa = a.str ? a.str->c_str() : "";
            const char* sb = b.str ? b.str->c_str() : "";
            return strcmp(sa, sb) == 0;
        }
        case VAL_TABLE: return a.tab.get() == b.tab.get();   // 表按引用身份比较
        case VAL_FUNC:  return a.fn.get() == b.fn.get();     // 函数按闭包身份比较
        default:        return false;
    }
}

// 类型名字符串（与 type() 内置函数对应）
inline const char* typeName(ValueKind k) {
    switch (k) {
        case VAL_NULL:   return "null";
        case VAL_INT:    return "int";
        case VAL_DOUBLE: return "float";
        case VAL_STR:    return "string";
        case VAL_BOOL:   return "bool";
        case VAL_TABLE:  return "table";
        case VAL_FUNC:   return "func";
        default:         return "unknown";
    }
}

// =============================================================================
//  Table 成员函数实现（此时 Value 已完整定义）
// =============================================================================
inline bool Table::get(const Value& key, Value& out) const {
    // 整数键：优先走数组段（支持连续 1-based 索引）
    if (key.kind == VAL_INT) {
        int64_t idx = key.i;
        if (idx >= 1 && idx <= (int64_t)arr.size()) {
            out = arr[(size_t)idx - 1];
            return true;
        }
    }
    auto it = mapv.find(toKey(key));
    if (it != mapv.end()) { out = it->second; return true; }
    return false;   // 未定义 → 返回 null（由调用方填充）
}

inline void Table::set(const Value& key, const Value& val) {
    if (key.kind == VAL_INT) {
        // 整数键 → 数组段（Lua 风格：允许尾部追加，中间空洞补 null）
        int64_t idx = key.i;
        if (idx < 1) throw runtime_error("表索引必须为正整数（1-based），得到 " + to_string(idx));
        // ★ P2-15：稀疏整数键（远超当前数组长度）改走哈希段，
        //   避免 t[1000000] = "x" 真的开出百万个槽
        if (idx <= (int64_t)arr.size() + 8) {
            size_t i = (size_t)idx - 1;
            if (i >= arr.size()) arr.resize(i + 1, Value());   // 空洞补 null
            arr[i] = val;
        } else {
            mapv[toKey(key)] = val;
        }
        return;
    }
    // 其余键 → 哈希段
    mapv[toKey(key)] = val;
}

inline size_t Table::length() const {
    // #t：连续整数键前缀长度（与 Lua 一致）
    size_t n = 0;
    for (size_t i = 0; i < arr.size(); i++) {
        if (arr[i].kind != VAL_NULL) n = i + 1;
        else break;
    }
    return n;
}

// =============================================================================
//  函数定义 / 调用帧
// =============================================================================
// ★ 上值描述：本闭包的某个上值从哪来
//    fromParentLocal = true  → 抓父帧的 locals[index]（引用捕获，共享变量）
//    fromParentLocal = false → 继承父闭包的第 index 个上值（多层嵌套的中转）
struct UpvalDesc {
    bool     fromParentLocal;
    uint16_t index;
};

struct FuncDef {
    uint16_t funcId;
    uint16_t paramCount;
    uint16_t localCount;
    uint32_t entryPC;
    uint32_t codeSize;
    std::vector<UpvalDesc> ups;   // ★ 闭包：上值描述表
};

struct CallFrame {
    uint16_t funcId;
    size_t   returnPC;
    size_t   stackBase;          // ★ P1-10：进入本函数时的操作数栈高度
    uint8_t  want;               // ★ P1-5：调用方希望得到几个返回值
    vector<Value> locals;
    // ★ 闭包支持
    shared_ptr<Closure> closure;                       // 本帧执行的闭包（可为空 = 普通函数）
    vector<pair<uint16_t, shared_ptr<Upvalue>>> openUps;  // 本帧里打开的上值：slot → Upvalue

    // 帧退场前必须把所有开放上值"关掉"（否则闭包会指向已释放的 locals）
    void closeUpvalues() {
        for (auto& kv : openUps) kv.second->close();
        openUps.clear();
    }
};

// ★ 1.7：读一个 FUNC_DEF 头。次版本 ≥7 时头里多一张上值描述表
//   （闭包要按这张表在【创建时刻】抓父帧的局部变量）
static size_t readFuncHeader(const std::vector<uint8_t>& c, size_t p, FuncDef& fd, int minor) {
    fd.funcId     = readU16(c, p);
    fd.paramCount = readU16(c, p);
    fd.localCount = readU16(c, p);
    fd.codeSize   = readU32(c, p);
    fd.ups.clear();
    if (minor >= 7) {
        uint16_t n = readU16(c, p);
        for (uint16_t i = 0; i < n; i++) {
            uint8_t  fromLocal = readU8(c, p);
            uint16_t index     = readU16(c, p);
            fd.ups.push_back({fromLocal != 0, index});
        }
    }
    fd.entryPC = (uint32_t)p;
    return p;
}

// ★ P1-6：exit(n) 用这个异常直接结束进程并带上退出码
struct ExitException { int code; };

// ★ 打不开 .aeo 不是「运行错误」，是用法/IO 错误：
//   单独一个类型，好在 main 里分成 E0002 + 退出码 3（和 ae -h 里写的退出码表一致）
struct LoadIOError : std::runtime_error {
    explicit LoadIOError(const std::string& m) : std::runtime_error(m) {}
};

// ★ P1-12 / P2-11：UTF-8 工具
//   把字符串按【码点】切块，返回每块的 (偏移, 字节数)
static std::vector<std::pair<size_t,size_t>> utf8Chars(const std::string& s) {
    std::vector<std::pair<size_t,size_t>> out;
    for (size_t p = 0; p < s.size(); ) {
        unsigned char c = (unsigned char)s[p];
        size_t len = (c < 0x80) ? 1
                   : ((c & 0xE0) == 0xC0) ? 2
                   : ((c & 0xF0) == 0xE0) ? 3
                   : ((c & 0xF8) == 0xF0) ? 4 : 1;
        if (p + len > s.size()) len = 1;
        out.push_back({p, len});
        p += len;
    }
    return out;
}
// ★ P1-12：UTF-8 字符数
static int64_t utf8Length(const std::string& s) {
    return (int64_t)utf8Chars(s).size();
}
// ★ P2-11：第 i 个字符（1-based，按码点），越界返回空串
static std::string utf8CharAt(const std::string& s, int64_t i) {
    if (i < 1) return std::string();
    auto cs = utf8Chars(s);
    if ((size_t)i > cs.size()) return std::string();
    return s.substr(cs[(size_t)i - 1].first, cs[(size_t)i - 1].second);
}
// ★ P2-9：浮点最短往返表示。此前 prln(x) 走默认流格式、str(x) 走 fixed 6 位小数，
//   同一个值会给出两种字符串（1e20 → "1e+20" vs "100000000000000000000.0"）。
//   现在两者共用这一条规则，并保证 float(str(x)) == x。
static std::string fmtDouble(double d) {
    if (d != d) return "nan";
    if (d == (double)INFINITY)  return "inf";
    if (d == -(double)INFINITY) return "-inf";
    char buf[64];
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(buf, sizeof(buf), "%.*g", prec, d);
        if (strtod(buf, nullptr) == d) break;
    }
    std::string s = buf;
    if (s.find_first_of(".eEni") == std::string::npos) s += ".0";   // 与 int 可区分
    return s;
}

// =============================================================================
//  虚拟机
// =============================================================================
class VM {
public:
    vector<Const> constPool;
    vector<uint8_t> code;
    size_t pc = 0;

    vector<Value> stack;
    vector<Value> globals;
    vector<FuncDef> funcs;

    vector<CallFrame> callStack;
    vector<Value>* currentLocals = nullptr;

    // ★ P1-2：字节码自带的调试信息与校验用数据
    string srcName = "<source>";
    string scriptDir;                                  // .aeo 所在目录（搜库用）
    string srcPath;                                    // .aeo 的完整路径（脚本可查）
    vector<string> scriptArgs;                         // 传给脚本的命令行参数
    vector<pair<uint16_t,std::string>> funcNames;
    struct LineEntry { uint32_t off, line, col; uint16_t file; };
    vector<string> fileNames;                       // ★ 源文件表（模块编译后不止一个）
    map<int,string> srcCache;                       // 每个源文件的文本（诊断摘录用）
    vector<LineEntry> lineTable;                    // 按 offset 升序
    size_t instrPC = 0;                             // 当前正在执行的指令起点
    // ★ 错误捕获：处理器栈（帧深度 / 操作数栈高度 / catch 落地点）
    struct Handler { size_t frameDepth, stackSize, landing; };
    vector<Handler> handlers;
    string thrownCode;                              // 显式抛出时指定的错误码
    string srcText;                                 // 源码（出错时摘录那一行）
    bool   srcLoaded = false;
    // ★ P1-13：调用深度上限（可用 --max-depth 调整）
    size_t maxCallDepth = 10000;
    int    exitCode = 0;

    // ★ 原生库状态
    map<string, const AeNativeDef*> nativeFns;    // "io.readFile" 与 "readFile"
    vector<aehost::LoadedModule>     nativeModules;
    vector<shared_ptr<string>>       nativePoolStr;   // 一次原生调用期间保活
    vector<shared_ptr<Table>>        nativePoolTab;
    int    bcMinor = 6;                               // ★ 字节码次版本（决定 FUNC_DEF 头有没有上值表）
    string pendingNativeError;                        // raise() 设置，返回后转成运行错误
    string nativeCallError;                           // ★ ABI v4：func_call 失败原因（借用给库）
    vector<Value> nativeArgs;                         // ★ ABI v4：本次原生调用的实参
                                                      //   （参数在调用前就弹栈了，func_retain
                                                      //    要靠它把裸指针恢复成 shared_ptr）
    vector<shared_ptr<Closure>> funcHandles;          // ★ ABI v4：库持有的闭包句柄（下标+1 = handle）
    string nativeLastError;                           // set_error() 设置，lastError() 读取

    // AeValue ↔ 内部 Value 的转换（ABI 边界的唯一通道）
    AeValue toAeValue(const Value& v) {
        AeValue a; a.kind = (int)v.kind; a.i = 0; a.f = 0.0; a.p = nullptr;
        switch (v.kind) {
            case VAL_INT:
            case VAL_BOOL:   a.i = v.i; break;
            case VAL_DOUBLE: a.f = v.f; break;
            case VAL_STR:    a.p = v.str.get(); break;
            case VAL_TABLE:  a.p = v.tab.get(); break;
            case VAL_FUNC:   a.p = v.fn.get();  break;   // ★ ABI v4：借用，库不该保存它
            default: break;
        }
        return a;
    }
    Value fromAeValue(const AeValue& a) {
        switch (a.kind) {
            case AE_FUNC:   throw runtime_error("原生库不能把函数值当参数回传（请用 func_retain 拿句柄）");
            case AE_INT:    return Value((int64_t)a.i);
            case AE_BOOL:   return Value(a.i != 0);
            case AE_FLOAT:  return Value((double)a.f);
            case AE_STRING: {
                const string* s = (const string*)a.p;
                return Value(make_shared<string>(s ? *s : string()));
            }
            case AE_TABLE: {
                const Table* tp = (const Table*)a.p;
                for (const auto& sp : nativePoolTab)
                    if (sp.get() == tp) return Value::makeTable(sp);
                throw runtime_error("原生库返回了非法的表引用");
            }
            default: return Value();
        }
    }
    Table* findPoolTable(const AeValue& a) {
        const Table* tp = (const Table*)a.p;
        for (const auto& sp : nativePoolTab)
            if (sp.get() == tp) return sp.get();
        return nullptr;
    }

    // 加载 .aeo 里声明的原生库
    void loadNativeModules(const vector<string>& names) {
        for (const string& nm : names) {
            aehost::LoadedModule lm;
            vector<string> tried;
            if (!aehost::loadLibrary(nm, scriptDir, lm, tried))
                throw runtime_error(aehost::triedText(nm, tried));
            if (lm.mod->abiVersion != AE_ABI_VERSION)
                throw runtime_error("库 " + nm + " 的 ABI 版本是 " + to_string(lm.mod->abiVersion) +
                                    "，本 VM 支持 " + to_string(ABI_VERSION_EXPECTED) + "（请重新编译该库）");
            if (lm.mod->setApi) lm.mod->setApi(aeNativeApi());
            int n = 0;
            const AeNativeDef* defs = aehost::readExports(lm, n);
            if (!defs || n <= 0) throw runtime_error("库 " + nm + " 没有导出任何函数");
            for (int i = 0; i < n; i++) {
                string key = nm + "." + defs[i].name;
                nativeFns[key] = &defs[i];
                if (!nativeFns.count(defs[i].name)) nativeFns[defs[i].name] = &defs[i];
            }
            nativeModules.push_back(lm);
        }
    }

    // ★ P2-12：%c 用得到
static std::string utf8Encode(unsigned cp) {
    std::string s;
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) {
        s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F));
    } else {
        s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F));
        s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F));
    }
    return s;
}

// 指令偏移 → (行, 列)
    void posForPC(size_t p, int& line, int& col, int* fileIdx = nullptr) const {
        line = 0; col = 1;
        if (fileIdx) *fileIdx = 0;
        if (lineTable.empty()) return;
        size_t lo = 0, hi = lineTable.size();
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (lineTable[mid].off <= p) lo = mid + 1;
            else hi = mid;
        }
        const LineEntry& e = (lo == 0) ? lineTable[0] : lineTable[lo - 1];
        line = (int)e.line;
        col  = (int)e.col;
        if (fileIdx) *fileIdx = (int)e.file;
    }
    int lineForPC(size_t p) const { int l, c; posForPC(p, l, c); return l; }
    string fileNameOf(int idx) const {
        if (idx >= 0 && (size_t)idx < fileNames.size()) return fileNames[(size_t)idx];
        return srcName;
    }
    // ★ 出错时按需读取源码；模块编译后每个文件各自缓存一份
    const string& sourceTextOf(int idx) {
        auto it = srcCache.find(idx);
        if (it != srcCache.end()) return it->second;
        string nm = fileNameOf(idx);
        string text;
        std::ifstream sf(nm, ios::binary);
        if (!sf && !scriptDir.empty()) {
            string base = nm;
            size_t s = base.find_last_of("/\\");
            if (s != string::npos) base = base.substr(s + 1);
            sf.open(scriptDir + "/" + base, ios::binary);
        }
        if (sf) { std::stringstream ss; ss << sf.rdbuf(); text = ss.str(); }
        return srcCache.emplace(idx, text).first->second;
    }
    const string& sourceText() { return sourceTextOf(0); }
    string funcNameOf(uint16_t fid) const {
        for (const auto& fn : funcNames)
            if (fn.first == fid) return fn.second;
        return "?";
    }
    // ★ P1-1：运行期错误的调用栈回溯
    //   每帧显示的是「该帧当前挂起在哪一行」：
    //     最内层 → 出错指令所在行；其余 → 它调用下一层的那一行
    std::string stackTrace() {
        std::string s;
        size_t top = callStack.size();
        for (size_t i = top; i-- > 0; ) {
            const CallFrame& fr = callStack[i];
            size_t p = (i + 1 == top) ? instrPC : callStack[i + 1].returnPC;
            int ln = 0, cl = 1, fi = 0;
            posForPC(p, ln, cl, &fi);
            // ★ 和主诊断一样按【显示列】报（中文算 2 列），别一处字节列一处显示列
            if (ln > 0) {
                std::string raw = aediag::lineAt(sourceTextOf(fi), ln);
                if (!raw.empty()) cl = aediag::displayCol(raw, cl, nullptr) + 1;
            }
            s += funcNameOf(fr.funcId);
            if (ln > 0) {
                s += "  " + fileNameOf(fi) + ":" + to_string(ln) + ":" + to_string(cl);
            }
            s += "\n";
        }
        return s;
    }

    Value pop() {
        if (stack.empty()) throw runtime_error("栈下溢");
        Value v = stack.back(); stack.pop_back(); return v;
    }
    void push(const Value& v) { stack.push_back(v); }

    // ★ P0-7：转换失败不再静默返回 0，而是抛出可读的运行时错误
    int64_t toInt(const Value& v) {
        switch (v.kind) {
            case VAL_INT:  return v.i;
            case VAL_BOOL: return v.i;      // 显式 int(true) → 1 仍然允许（这是 escape hatch）
            case VAL_DOUBLE:
                // 超出 int64 范围的 double 转整数是 UB，明确报错
                if (v.f != v.f || v.f >= 9223372036854775808.0 || v.f < -9223372036854775808.0)
                    throw runtime_error("int(): 浮点值 " + toString(v) + " 超出 int64 范围");
                return (int64_t)v.f;
            case VAL_STR: {
                const string& s = v.str ? *v.str : string();
                char c0 = s.empty() ? '\0' : s[0];
                bool okStart = (c0 >= '0' && c0 <= '9') || c0 == '-' || c0 == '+';
                size_t used = 0;
                int64_t out = 0;
                bool ok = false;
                if (okStart) {
                    try { out = (int64_t)stoll(s, &used); ok = (used == s.size()); }
                    catch (...) { ok = false; }
                }
                if (!ok) throw runtime_error("int(): 无法把 \"" + s + "\" 解析为整数");
                return out;
            }
            default:
                throw runtime_error(string("int() 不支持 ") + typeName(v.kind) + " 类型");
        }
    }

    double toDouble(const Value& v) {
        switch (v.kind) {
            case VAL_INT:  return (double)v.i;
            case VAL_BOOL: return (double)v.i;
            case VAL_DOUBLE: return v.f;
            case VAL_STR: {
                const string& s = v.str ? *v.str : string();
                char c0 = s.empty() ? '\0' : s[0];
                bool okStart = (c0 >= '0' && c0 <= '9') || c0 == '-' || c0 == '+' || c0 == '.';
                size_t used = 0;
                double out = 0;
                bool ok = false;
                if (okStart) {
                    try { out = stod(s, &used); ok = (used == s.size()); }
                    catch (...) { ok = false; }
                }
                if (!ok) throw runtime_error("float(): 无法把 \"" + s + "\" 解析为浮点数");
                return out;
            }
            default:
                throw runtime_error(string("float() 不支持 ") + typeName(v.kind) + " 类型");
        }
    }

    // ★ P0-5：环检测 + 深度上限。
    //   此前 `local t = {}  t[1] = t  prln(t)` 会让这里无限递归 →
    //   C++ 栈溢出 → 进程以 0xC00000FD 直接崩溃。现在打印闭环标记。
    string toString(const Value& v) {
        vector<const Table*> path;
        return strOf(v, path);
    }

    string strOf(const Value& v, vector<const Table*>& path) {
        switch (v.kind) {
            case VAL_NULL:  return "null";
            case VAL_STR:   return v.str ? *v.str : "";
            case VAL_BOOL:  return v.i ? "true" : "false";
            case VAL_INT:   return to_string(v.i);
            case VAL_FUNC:  return v.fn ? ("<func #" + to_string(v.fn->funcId) + ">") : "<func>";
            case VAL_TABLE: {
                const Table* t = v.tab.get();
                if (!t) return "{}";
                for (const Table* p : path)
                    if (p == t) return "<cycle>";        // 自引用 / 环形引用
                if (path.size() >= 64) return "<deep>";  // 极深的非环表也保护一下
                path.push_back(t);
                string s = "{";
                // 数组段（按 1-based 连续输出）
                for (size_t i = 0; i < t->arr.size(); i++) {
                    if (i > 0) s += ", ";
                    s += strOf(t->arr[i], path);
                }
                // 哈希段
                for (const auto& kv : t->mapv) {
                    if (!s.empty() && s.back() != '{') s += ", ";
                    const TableKey& k = kv.first;
                    if (k.kind == VAL_STR)      s += "\"" + string(k.str ? k.str->c_str() : "") + "\"";
                    else if (k.kind == VAL_INT) s += to_string(k.i);
                    else if (k.kind == VAL_BOOL) s += (k.i ? "true" : "false");
                    else if (k.kind == VAL_DOUBLE) s += to_string(k.f);
                    s += " = " + strOf(kv.second, path);
                }
                s += "}";
                path.pop_back();
                return s;
            }
            case VAL_DOUBLE: {
                // ★ P2-9：与 printValue 共用同一条最短往返规则
                return fmtDouble(v.f);
            }
            default: return "?";
        }
    }

    // 真值判断：null / false / 0 / 0.0 / "" / 空数组 均为假
    bool isFalse(const Value& v) const {
        switch (v.kind) {
            case VAL_NULL:  return true;
            case VAL_BOOL:  return v.i == 0;
            case VAL_INT:   return v.i == 0;
            case VAL_DOUBLE: return v.f == 0.0;
            case VAL_STR:    return v.str == nullptr || *v.str == "";
            case VAL_TABLE:  return v.tab == nullptr || (v.tab->arr.empty() && v.tab->mapv.empty());
            default:         return false;
        }
    }

    // =========================================================================
    //  ★ 动态算术 / 严格比较（运行期按值的实际类型分派）
    //     编译器只做“粗略”的静态类型猜测，语义一律以运行期类型为准，
    //     因此整数族指令与浮点族指令行为完全等价。
    // =========================================================================
    static bool isNumeric(const Value& v) {
        // ★ P0-4：bool 不是数值 —— 它有自己的 type()，且判等时与 int 分家，
        //   若还允许 true + 1 = 2，就会出现“能相加却不相等”的自相矛盾
        return v.kind == VAL_INT || v.kind == VAL_DOUBLE;
    }
    static void requireNumeric(const Value& v, const char* opName) {
        if (!isNumeric(v))
            throw runtime_error(string("运算符 ") + opName + " 不支持 " +
                                typeName(v.kind) + " 类型" +
                                (v.kind == VAL_BOOL ? "（bool 不参与算术，可先 int(x) 显式转换）" : ""));
    }

    // ★ P0-6：整数回绕语义。C++ 有符号溢出是 UB，直接用 `a + b` 会被优化器
    //   按“溢出不可能发生”处理；这里用无符号中转实现确定的二进制补码回绕。
    static int64_t wrapAdd(int64_t a, int64_t b) {
        uint64_t u = (uint64_t)a + (uint64_t)b;
        int64_t r; memcpy(&r, &u, 8); return r;
    }
    static int64_t wrapSub(int64_t a, int64_t b) {
        uint64_t u = (uint64_t)a - (uint64_t)b;
        int64_t r; memcpy(&r, &u, 8); return r;
    }
    static int64_t wrapMul(int64_t a, int64_t b) {
        uint64_t u = (uint64_t)a * (uint64_t)b;
        int64_t r; memcpy(&r, &u, 8); return r;
    }

    // ★ P2-1：位运算只接受 int（bool 不是数值；float 请显式 int(x)）
    static void requireInt(const Value& v, const char* opName) {
        if (v.kind != VAL_INT)
            throw runtime_error(string("位运算符 ") + opName + " 只支持 int，得到 " +
                                typeName(v.kind) +
                                (v.kind == VAL_DOUBLE ? "（float 请用 int(x) 显式转换）" : ""));
    }

    Value doAdd(const Value& l, const Value& r) {
        // 只要任一侧是字符串 → 拼接（任意值转字符串）
        if (l.kind == VAL_STR || r.kind == VAL_STR)
            return Value(toString(l) + toString(r));
        requireNumeric(l, "+"); requireNumeric(r, "+");
        if (l.kind == VAL_DOUBLE || r.kind == VAL_DOUBLE)
            return Value(toDouble(l) + toDouble(r));
        return Value(wrapAdd(l.i, r.i));
    }
    Value doSub(const Value& l, const Value& r) {
        requireNumeric(l, "-"); requireNumeric(r, "-");
        if (l.kind == VAL_DOUBLE || r.kind == VAL_DOUBLE)
            return Value(toDouble(l) - toDouble(r));
        return Value(wrapSub(l.i, r.i));
    }
    Value doMul(const Value& l, const Value& r) {
        requireNumeric(l, "*"); requireNumeric(r, "*");
        if (l.kind == VAL_DOUBLE || r.kind == VAL_DOUBLE)
            return Value(toDouble(l) * toDouble(r));
        return Value(wrapMul(l.i, r.i));
    }
    Value doDiv(const Value& l, const Value& r) {
        requireNumeric(l, "/"); requireNumeric(r, "/");
        if (l.kind == VAL_DOUBLE || r.kind == VAL_DOUBLE) {
            double rd = toDouble(r);
            if (rd == 0.0) throw runtime_error("浮点除以零");
            return Value(toDouble(l) / rd);
        }
        if (r.i == 0) throw runtime_error("整数除以零");
        // INT64_MIN / -1 在 C++ 里是 UB（结果数学上等于 2^63，回绕到 INT64_MIN）
        if (l.i == INT64_MIN && r.i == -1) return Value(INT64_MIN);
        // ★ P2-10：向下取整除法（-7 / 3 == -3），与 Python/Lua 一致
        int64_t q = l.i / r.i;
        if ((l.i % r.i != 0) && ((l.i < 0) != (r.i < 0))) q--;
        return Value(q);
    }
    Value doMod(const Value& l, const Value& r) {
        requireNumeric(l, "%"); requireNumeric(r, "%");
        if (l.kind == VAL_DOUBLE || r.kind == VAL_DOUBLE) {
            double rd = toDouble(r);
            if (rd == 0.0) throw runtime_error("取模除以零");
            return Value(fmod(toDouble(l), rd));
        }
        if (r.i == 0) throw runtime_error("取模除以零");
        if (l.i == INT64_MIN && r.i == -1) return Value((int64_t)0);   // 同样是 UB，显式定义
        // ★ P2-10：结果与除数同号（正除数时总为非负），配合向下取整除法
        int64_t m = l.i % r.i;
        if (m != 0 && ((m < 0) != (r.i < 0))) m += r.i;
        return Value(m);
    }

    // 比较（整数族 0x10-0x15 与 浮点族 0x16-0x1B 共用）
    //   ★ P0-3 定案语义（判等与大小比较刻意不同）：
    //     == / != ：
    //       · 与 null 比较永远合法 —— null 表示“无值”，只与自身相等（false / true）
    //       · int 与 float 之间【报类型错】：两者是同一数值域的不同表示，
    //         静默判为不等是最容易踩的坑（写 1.0 或 float(x) 即可）
    //       · 其余类型不同 → false / true（不同类型本来就不相等）
    //     < <= > >= ：
    //       · 类型不同 → 报类型错（大小比较必须有可比性）
    //       · 同类型里 null / 表没有大小关系 → 报类型错
    void doCompare(uint8_t op) {
        Value r = pop(), l = pop();
        bool isEq  = (op == 0x10 || op == 0x16);
        bool isNeq = (op == 0x11 || op == 0x17);
        bool isLt  = (op == 0x12 || op == 0x18);
        bool isLe  = (op == 0x13 || op == 0x19);
        bool isGt  = (op == 0x14 || op == 0x1A);

        bool t;
        if (isEq || isNeq) {
            if (l.kind == VAL_NULL || r.kind == VAL_NULL) {
                t = (l.kind == VAL_NULL && r.kind == VAL_NULL);
            } else if (l.kind != r.kind) {
                bool mixed = (l.kind == VAL_INT && r.kind == VAL_DOUBLE) ||
                             (l.kind == VAL_DOUBLE && r.kind == VAL_INT);
                if (mixed)
                    throw runtime_error("不能直接比较 int 与 float：请显式统一类型，"
                                        "例如 float(x) 或把字面量写成 1.0");
                t = false;                       // 不同类型即不相等
            } else {
                t = valueEquals(l, r);           // 表按引用身份比较
            }
            if (isNeq) t = !t;
        } else {
            if (l.kind != r.kind)
                throw runtime_error(string("大小比较要求两侧类型相同，得到 ") +
                                    typeName(l.kind) + " 与 " + typeName(r.kind));
            bool ordered = true;
            int  cmp = 0;
            switch (l.kind) {
                case VAL_STR: {
                    const char* sa = l.str ? l.str->c_str() : "";
                    const char* sb = r.str ? r.str->c_str() : "";
                    cmp = strcmp(sa, sb);
                    break;
                }
                case VAL_DOUBLE:
                    if (l.f != l.f || r.f != r.f) ordered = false;   // NaN
                    else cmp = (l.f < r.f) ? -1 : (l.f > r.f ? 1 : 0);
                    break;
                case VAL_INT:
                case VAL_BOOL:
                    cmp = (l.i < r.i) ? -1 : (l.i > r.i ? 1 : 0);
                    break;
                default:                        // null / 表：无大小关系
                    throw runtime_error(string("类型 ") + typeName(l.kind) +
                                        " 不支持大小比较");
            }
            t = !ordered ? false
              : isLt ? (cmp <  0)
              : isLe ? (cmp <= 0)
              : isGt ? (cmp >  0)
              :        (cmp >= 0);          // GE
        }
        push(Value(t));
    }

    // ★ P1-5：把栈顶 produced 个值调整成恰好 want 个
    //   want=255 表示"全部保留"（预留给将来的多值展开）
    void adjustTop(size_t produced, uint8_t want) {
        if (want == 255) return;
        if (produced > (size_t)want) {
            stack.resize(stack.size() - (produced - want));
        } else {
            for (size_t i = produced; i < (size_t)want; i++) stack.push_back(Value());
        }
    }

    // =========================================================================
    //  ★ P2-11/12/14/16：内建工具
    // =========================================================================
    vector<Value> takeArgs(uint8_t argc) {
        vector<Value> a(argc);
        for (uint8_t i = 0; i < argc; i++) a[argc - 1 - i] = pop();
        return a;
    }
    static const string& asStr(const Value& v, const char* fn) {
        static const string kEmpty;
        if (v.kind != VAL_STR)
            throw runtime_error(string(fn) + " 需要字符串参数，得到 " + typeName(v.kind));
        return v.str ? *v.str : kEmpty;
    }
    static int64_t asInt(const Value& v, const char* fn) {
        if (v.kind != VAL_INT)
            throw runtime_error(string(fn) + " 需要 int 参数，得到 " + typeName(v.kind));
        return v.i;
    }
    static shared_ptr<Table> asTable(const Value& v, const char* fn) {
        if (!v.isTable())
            throw runtime_error(string(fn) + " 需要表参数，得到 " + typeName(v.kind));
        return v.tab;
    }
    static void wantArgs(size_t got, size_t need, const char* fn) {
        if (got != need)
            throw runtime_error(string(fn) + " 需要 " + to_string(need) + " 个参数，得到 " + to_string(got));
    }

    // 深拷贝（环安全）
    Value deepCopy(const Value& v, map<const Table*, Value>& seen) {
        if (v.kind != VAL_TABLE) return v;      // 标量与字符串不可变，可直接共享
        const Table* t = v.tab.get();
        if (!t) return v;
        auto it = seen.find(t);
        if (it != seen.end()) return it->second;
        shared_ptr<Table> nt = make_shared<Table>();
        Value nv = Value::makeTable(nt);
        seen[t] = nv;
        nt->arr.resize(t->arr.size());
        for (size_t i = 0; i < t->arr.size(); i++) nt->arr[i] = deepCopy(t->arr[i], seen);
        for (const auto& kv : t->mapv) nt->mapv[kv.first] = deepCopy(kv.second, seen);
        return nv;
    }
    // 深相等（环安全：已在比较中的对视为相等）
    bool deepEqual(const Value& a, const Value& b, set<pair<const Table*,const Table*>>& seen) {
        if (a.kind != b.kind) return false;
        if (a.kind != VAL_TABLE) return valueEquals(a, b);
        const Table* ta = a.tab.get();
        const Table* tb = b.tab.get();
        if (ta == tb) return true;
        if (!ta || !tb) return false;
        auto key = make_pair(ta, tb);
        if (seen.count(key)) return true;
        seen.insert(key);
        if (ta->arr.size() != tb->arr.size() || ta->mapv.size() != tb->mapv.size()) return false;
        for (size_t i = 0; i < ta->arr.size(); i++)
            if (!deepEqual(ta->arr[i], tb->arr[i], seen)) return false;
        for (const auto& kv : ta->mapv) {
            auto it = tb->mapv.find(kv.first);
            if (it == tb->mapv.end()) return false;
            if (!deepEqual(kv.second, it->second, seen)) return false;
        }
        return true;
    }
    // 表中非 null 条目总数（区别于 len() 只数连续前缀）
    static int64_t tableCount(const Table* t) {
        int64_t n = 0;
        for (const Value& v : t->arr) if (v.kind != VAL_NULL) n++;
        for (const auto& kv : t->mapv) if (kv.second.kind != VAL_NULL) n++;
        return n;
    }
    // ★ P2-12：printf 风格格式化
    string formatString(const string& f, const vector<Value>& args) {
        string out;
        size_t ai = 0;
        for (size_t i = 0; i < f.size(); ) {
            if (f[i] != '%') { out += f[i++]; continue; }
            size_t j = i + 1;
            if (j < f.size() && f[j] == '%') { out += '%'; i = j + 1; continue; }
            string spec = "%";
            while (j < f.size() && strchr("-+ #0", f[j])) spec += f[j++];
            int width = 0;
            while (j < f.size() && isdigit((unsigned char)f[j])) { width = width * 10 + (f[j] - '0'); spec += f[j++]; }
            int prec = 0;
            if (j < f.size() && f[j] == '.') {
                spec += f[j++];
                while (j < f.size() && isdigit((unsigned char)f[j])) { prec = prec * 10 + (f[j] - '0'); spec += f[j++]; }
            }
            if (j >= f.size()) throw runtime_error("format: 格式串以 % 结尾");
            char conv = f[j++];
            if (ai >= args.size())
                throw runtime_error("format: 格式串需要更多参数（已用 " + to_string(ai) + " 个）");
            const Value& a = args[ai++];
            string sv;
            size_t need = (size_t)width + (size_t)prec + 256;
            switch (conv) {
                case 'd': case 'i': case 'x': case 'X': case 'o': case 'u':
                    sv = spec + "ll" + conv;
                    { vector<char> buf(need);
                      snprintf(buf.data(), buf.size(), sv.c_str(), (long long)toInt(a));
                      out += buf.data(); }
                    break;
                case 'f': case 'e': case 'E': case 'g': case 'G':
                    sv = spec + conv;
                    { vector<char> buf(need);
                      snprintf(buf.data(), buf.size(), sv.c_str(), toDouble(a));
                      out += buf.data(); }
                    break;
                case 's': {
                    string s2 = toString(a);
                    need += s2.size();
                    sv = spec + "s";
                    vector<char> buf(need);
                    snprintf(buf.data(), buf.size(), sv.c_str(), s2.c_str());
                    out += buf.data();
                    break;
                }
                case 'c': {
                    unsigned cp = (unsigned)toInt(a);
                    out += utf8Encode(cp);
                    break;
                }
                default: throw runtime_error(string("format: 不支持的转换 %") + conv);
            }
            i = j;
        }
        return out;
    }

    // =========================================================================
    //  ★ 内置函数分派
    //      0=pr  1=inp  2=prln  3=len  4=str  5=int  6=float  7=type
    //      8=assert  9=error  10=exit
    //     11=substr 12=ord 13=chr 14=find 15=upper 16=lower 17=trim
    //     18=replace 19=split 20=join 21=repeat 22=format
    //     23=delete 24=insert 25=pop 26=append 27=count
    //     28=copy 29=deepcopy 30=equal
    // =========================================================================
    void invoke(uint8_t funcId, uint8_t argc, uint8_t want) {
        size_t produced = 0;
        if (funcId == 0 || funcId == 2) {
            vector<Value> args(argc);
            for (uint8_t i = 0; i < argc; i++) args[argc - 1 - i] = pop();
            for (uint8_t i = 0; i < argc; i++) {
                if (i > 0) cout << " ";
                printValue(args[i]);
            }
            if (funcId == 2) cout << endl;
        } else if (funcId == 1) {                 // inp([提示])
            string prompt;
            if (argc > 0) prompt = toString(pop());
            if (!prompt.empty()) { cout << prompt << flush; }   // ★ P2-28：提示真正输出
            string line;
            if (getline(cin, line)) push(Value(make_shared<string>(line)));
            else                    push(Value());          // ★ EOF → null
            produced = 1;
        } else if (funcId == 3) {                 // len(x)
            Value v = pop();
            int64_t len = 0;
            if (v.kind == VAL_TABLE)      len = (int64_t)v.tab->length();
            else if (v.kind == VAL_STR)   len = v.str ? utf8Length(*v.str) : 0;  // ★ P1-12：按字符
            else if (v.kind == VAL_INT) {
                int64_t n = v.i; if (n < 0) n = -n;
                if (n == 0) len = 1; else { while (n > 0) { len++; n /= 10; } }
            }
            // ★ P2-13：float / bool / null 不再静默返回 0
            else throw runtime_error(string("len() 不支持 ") + typeName(v.kind) +
                                     " 类型（仅支持 string / table / int 的位数）");
            push(Value(len));
            produced = 1;
        } else if (funcId == 4) {                 // str(x)
            push(Value(make_shared<string>(toString(pop()))));
            produced = 1;
        } else if (funcId == 5) {                 // int(x)
            push(Value(toInt(pop())));
            produced = 1;
        } else if (funcId == 6) {                 // float(x)
            push(Value(toDouble(pop())));
            produced = 1;
        } else if (funcId == 7) {                 // type(x) ★
            Value v = pop();
            // ★ 必须显式构造 std::string：const char* → bool 是标准转换，
            //   会优先于 const char* → std::string 的用户自定义转换，
            //   之前这里恒压入 true。
            push(Value(string(typeName(v.kind))));
            produced = 1;
        } else if (funcId == 8) {                 // ★ P1-6：assert(cond [, msg])
            vector<Value> args(argc);
            for (uint8_t i = 0; i < argc; i++) args[argc - 1 - i] = pop();
            if (args.empty()) throw runtime_error("assert() 需要至少 1 个参数");
            if (isFalse(args[0])) {
                string msg = (args.size() > 1) ? toString(args[1]) : "断言失败";
                thrownCode = AE_E_ASSERT;
                throw runtime_error("assert 失败: " + msg);
            }
            push(args[0]);                        // 断言通过 → 返回条件本身（便于链式使用）
            produced = 1;
        } else if (funcId == 9) {                 // ★ P1-6：error(msg)
            Value v = argc > 0 ? pop() : Value();
            thrownCode = AE_E_USER_ERROR;
            throw runtime_error(argc > 0 ? toString(v) : "脚本主动报错");
        } else if (funcId == 31) {                // ★ 错误捕获：raise(e) 重新抛出
            Value v = argc > 0 ? pop() : Value();
            string msg;
            if (v.isTable()) {
                Value mv, cv;
                if (v.tab->get(Value(string("msg")), mv))   msg = toString(mv);
                if (v.tab->get(Value(string("code")), cv))  thrownCode = toString(cv);
            } else {
                msg = toString(v);
            }
            if (msg.empty()) msg = "raise() 重新抛出了一个错误";
            if (thrownCode.empty()) thrownCode = AE_E_USER_ERROR;
            throw runtime_error(msg);
        } else if (funcId == 10) {                // ★ P1-6：exit([code])
            Value v = argc > 0 ? pop() : Value();
            int code = 0;
            if (argc > 0) code = (int)toInt(v);
            throw ExitException{code};
        } else if (funcId == 22) {                // ★ P2-12：format(fmt, ...)
            vector<Value> a = takeArgs(argc);
            if (a.empty()) throw runtime_error("format() 需要至少 1 个参数");
            string fmt = asStr(a[0], "format");
            vector<Value> rest(a.begin() + 1, a.end());
            push(Value(make_shared<string>(formatString(fmt, rest))));
            produced = 1;
        } else if (funcId >= 11 && funcId <= 21) {   // ★ P2-11：字符串内建
            vector<Value> a = takeArgs(argc);
            string r;
            switch (funcId) {
                case 11: {   // substr(s, start, len) —— 按字符，越界自动裁剪
                    wantArgs(a.size(), 3, "substr");
                    const string& s = asStr(a[0], "substr");
                    int64_t st = asInt(a[1], "substr"), n = asInt(a[2], "substr");
                    if (st < 1) throw runtime_error("substr: 起始位置必须 >= 1");
                    if (n < 0) throw runtime_error("substr: 长度必须 >= 0");
                    auto cs = utf8Chars(s);
                    if ((size_t)st > cs.size() || n == 0) { r = ""; break; }
                    size_t from = cs[(size_t)st - 1].first;
                    size_t cnt  = std::min<size_t>((size_t)n, cs.size() - (size_t)st + 1);
                    size_t last = (size_t)st - 1 + cnt - 1;
                    size_t to   = cs[last].first + cs[last].second;
                    r = s.substr(from, to - from);
                    break;
                }
                case 12: {   // ord(s [, i]) —— 第 i 个字符的码点
                    if (a.size() < 1 || a.size() > 2) throw runtime_error("ord 需要 1~2 个参数");
                    const string& s = asStr(a[0], "ord");
                    int64_t i = (a.size() == 2) ? asInt(a[1], "ord") : 1;
                    string ch = utf8CharAt(s, i);
                    if (ch.empty()) throw runtime_error("ord: 位置 " + to_string(i) + " 越界");
                    unsigned char c0 = (unsigned char)ch[0];
                    unsigned cp = (c0 < 0x80) ? c0
                                : ((c0 & 0xE0) == 0xC0) ? (c0 & 0x1F)
                                : ((c0 & 0xF0) == 0xE0) ? (c0 & 0x0F) : (c0 & 0x07);
                    for (size_t k = 1; k < ch.size(); k++)
                        cp = (cp << 6) | ((unsigned char)ch[k] & 0x3F);
                    push(Value((int64_t)cp));
                    produced = 1;
                    break;
                }
                case 13: {   // chr(n)
                    wantArgs(a.size(), 1, "chr");
                    push(Value(make_shared<string>(utf8Encode((unsigned)asInt(a[0], "chr")))));
                    produced = 1;
                    break;
                }
                case 14: {   // find(s, sub [, start]) —— 字符序号，找不到返回 null
                    if (a.size() < 2 || a.size() > 3) throw runtime_error("find 需要 2~3 个参数");
                    const string& s = asStr(a[0], "find");
                    const string& sub = asStr(a[1], "find");
                    int64_t st = (a.size() == 3) ? asInt(a[2], "find") : 1;
                    if (st < 1) throw runtime_error("find: 起始位置必须 >= 1");
                    auto cs = utf8Chars(s);
                    size_t byteFrom = ((size_t)st <= cs.size()) ? cs[(size_t)st - 1].first : s.size();
                    size_t at = sub.empty() ? string::npos : s.find(sub, byteFrom);
                    if (at == string::npos) { push(Value()); }
                    else {
                        int64_t idx = 1;
                        for (const auto& c : cs) { if (c.first >= at) break; idx++; }
                        push(Value(idx));
                    }
                    produced = 1;
                    break;
                }
                case 15: case 16: {   // upper / lower（仅 ASCII）
                    wantArgs(a.size(), 1, funcId == 15 ? "upper" : "lower");
                    r = asStr(a[0], funcId == 15 ? "upper" : "lower");
                    for (char& ch : r)
                        ch = (char)(funcId == 15 ? toupper((unsigned char)ch) : tolower((unsigned char)ch));
                    break;
                }
                case 17: {   // trim（去两端空白）
                    wantArgs(a.size(), 1, "trim");
                    const string& s = asStr(a[0], "trim");
                    size_t b = 0, e = s.size();
                    while (b < e && isspace((unsigned char)s[b])) b++;
                    while (e > b && isspace((unsigned char)s[e - 1])) e--;
                    r = s.substr(b, e - b);
                    break;
                }
                case 18: {   // replace(s, old, new)
                    wantArgs(a.size(), 3, "replace");
                    const string& s = asStr(a[0], "replace");
                    const string& o = asStr(a[1], "replace");
                    const string& nw = asStr(a[2], "replace");
                    if (o.empty()) throw runtime_error("replace: 被替换的子串不能为空");
                    r = s;
                    size_t p = 0;
                    while ((p = r.find(o, p)) != string::npos) {
                        r.replace(p, o.size(), nw);
                        p += nw.size();
                    }
                    break;
                }
                case 19: {   // split(s, sep) → 表
                    wantArgs(a.size(), 2, "split");
                    const string& s = asStr(a[0], "split");
                    const string& sep = asStr(a[1], "split");
                    if (sep.empty()) throw runtime_error("split: 分隔符不能为空");
                    shared_ptr<Table> t = make_shared<Table>();
                    size_t p = 0, q;
                    while ((q = s.find(sep, p)) != string::npos) {
                        t->arr.push_back(Value(make_shared<string>(s.substr(p, q - p))));
                        p = q + sep.size();
                    }
                    t->arr.push_back(Value(make_shared<string>(s.substr(p))));
                    push(Value::makeTable(t));
                    produced = 1;
                    break;
                }
                case 20: {   // join(t, sep)
                    wantArgs(a.size(), 2, "join");
                    shared_ptr<Table> t = asTable(a[0], "join");
                    const string& sep = asStr(a[1], "join");
                    for (size_t i = 0; i < t->arr.size(); i++) {
                        if (i > 0) r += sep;
                        r += toString(t->arr[i]);
                    }
                    break;
                }
                case 21: {   // repeat(s, n)
                    wantArgs(a.size(), 2, "repeat");
                    const string& s = asStr(a[0], "repeat");
                    int64_t n = asInt(a[1], "repeat");
                    if (n < 0) throw runtime_error("repeat: 次数必须 >= 0");
                    for (int64_t k = 0; k < n; k++) r += s;
                    break;
                }
                default: break;
            }
            // 11/15/16/17/18/20/21 返回字符串；12/13/14/19 已自行 push
            if (funcId != 12 && funcId != 13 && funcId != 14 && funcId != 19) {
                push(Value(make_shared<string>(r)));
                produced = 1;
            }
        } else if (funcId >= 23 && funcId <= 27) {   // ★ P2-14：表原语
            vector<Value> a = takeArgs(argc);
            if (funcId == 23) {                      // delete(t, k) → bool
                wantArgs(a.size(), 2, "delete");
                shared_ptr<Table> t = asTable(a[0], "delete");
                const Value& k = a[1];
                bool found = false;
                if (k.kind == VAL_INT && k.i >= 1 && k.i <= (int64_t)t->arr.size() &&
                    t->arr[(size_t)k.i - 1].kind != VAL_NULL) {
                    t->arr[(size_t)k.i - 1] = Value();
                    while (!t->arr.empty() && t->arr.back().kind == VAL_NULL) t->arr.pop_back();
                    found = true;
                } else {
                    auto it = t->mapv.find(toKey(k));
                    if (it != t->mapv.end()) { t->mapv.erase(it); found = true; }
                }
                push(Value(found));
                produced = 1;
            } else if (funcId == 24) {               // insert(t, pos, v) → 新长度
                wantArgs(a.size(), 3, "insert");
                shared_ptr<Table> t = asTable(a[0], "insert");
                int64_t pos = asInt(a[1], "insert");
                int64_t len = (int64_t)t->length();
                if (pos < 1 || pos > len + 1)
                    throw runtime_error("insert: 位置必须在 1.." + to_string(len + 1) + "，得到 " + to_string(pos));
                for (int64_t i = len; i >= pos; i--) {
                    Value tmp;
                    t->get(Value(i), tmp);
                    t->set(Value(i + 1), tmp);
                }
                t->set(Value(pos), a[2]);
                push(Value(len + 1));
                produced = 1;
            } else if (funcId == 25) {               // pop(t) → 末尾元素或 null
                wantArgs(a.size(), 1, "pop");
                shared_ptr<Table> t = asTable(a[0], "pop");
                int64_t len = (int64_t)t->length();
                if (len == 0) { push(Value()); }
                else {
                    Value out;
                    t->get(Value(len), out);
                    t->arr[(size_t)len - 1] = Value();
                    while (!t->arr.empty() && t->arr.back().kind == VAL_NULL) t->arr.pop_back();
                    push(out);
                }
                produced = 1;
            } else if (funcId == 26) {               // append(t, v) → 新长度
                wantArgs(a.size(), 2, "append");
                shared_ptr<Table> t = asTable(a[0], "append");
                int64_t n = (int64_t)t->length() + 1;
                t->mapv.erase(toKey(Value(n)));
                t->set(Value(n), a[1]);
                push(Value(n));
                produced = 1;
            } else {                                 // count(t) → 非 null 条目数
                wantArgs(a.size(), 1, "count");
                push(Value(tableCount(asTable(a[0], "count").get())));
                produced = 1;
            }
        } else if (funcId >= 28) {                   // ★ P2-16：拷贝与深相等
            vector<Value> a = takeArgs(argc);
            if (funcId == 28) {                      // copy(t) —— 浅拷贝
                wantArgs(a.size(), 1, "copy");
                shared_ptr<Table> t = asTable(a[0], "copy");
                shared_ptr<Table> nt = make_shared<Table>();
                nt->arr = t->arr;
                nt->mapv = t->mapv;
                push(Value::makeTable(nt));
                produced = 1;
            } else if (funcId == 29) {               // deepcopy(t) —— 深拷贝（环安全）
                wantArgs(a.size(), 1, "deepcopy");
                map<const Table*, Value> seen;
                push(deepCopy(a[0], seen));
                produced = 1;
            } else {                                 // equal(a, b) —— 深相等
                wantArgs(a.size(), 2, "equal");
                set<pair<const Table*,const Table*>> seen;
                push(Value(deepEqual(a[0], a[1], seen)));
                produced = 1;
            }
        } else {
            throw runtime_error("调用未定义的函数 id=" + to_string(funcId));
        }
        adjustTop(produced, want);
    }

    void printValue(const Value& v) {
        if (v.kind == VAL_TABLE)   cout << toString(v);
        else if (v.kind == VAL_STR) { if (v.str) cout << *v.str; }
        else if (v.kind == VAL_NULL) cout << "null";
        else if (v.kind == VAL_BOOL) cout << (v.i ? "true" : "false");
        else if (v.kind == VAL_DOUBLE) cout << fmtDouble(v.f);   // ★ P2-9：与 str() 一致
        else cout << v.i;
    }

    // ---------- 预处理函数表 ----------
    void preScanFunctions() {
        size_t savedPC = pc;
        pc = 0;
        while (pc < code.size()) {
            uint8_t op = code[pc++];
            if (op == 0x40) {
                if (pc + 11 > code.size()) break;
                FuncDef fd;
                readFuncHeader(code, pc, fd, bcMinor);
                if (fd.funcId >= funcs.size()) funcs.resize(fd.funcId + 1);
                funcs[fd.funcId] = fd;
                pc += fd.codeSize;
            } else if (op == 0x03) {
                // ★ HALT 不再终止预扫描：现在的布局是
                //   [入口 stub: CALL __script; HALT][FUNC_DEF ...]...
                //   若在此 break 就会漏掉后面所有函数定义
                continue;
            } else skipInstruction(op);
        }
        pc = savedPC;
    }

    void skipInstruction(uint8_t op) {
        switch (op) {
            case 0x01: case 0x04: case 0x05:
            case 0x30: case 0x31: case 0x2A: case 0x2B:
            case 0x50:   // NEW_TABLE u16(count) → 3 字节
                pc += 2; break;
            case 0x02:   // ★ P1-5：CALL u8(id) u8(argc) u8(want) → 3 字节操作数
                pc += 3; break;
            case 0x41:   // ★ P1-5：CALL_FUNC u16(id) u8(argc) u8(want) → 4 字节操作数
                pc += 4; break;
            case 0x44:   // ★ 1.7：CLOSURE u16(funcId) u8(nups) → 3 字节
                pc += 3; break;
            case 0x45:   // ★ 1.7：GET_UPVAL u16(i) → 2 字节
            case 0x46:   // ★ 1.7：SET_UPVAL u16(i)
            case 0x47:   // ★ 1.7：CLOSE_SLOT u16(slot)
            case 0x48:   // ★ 1.7：CALL_VALUE u8(argc) u8(want)
                pc += 2; break;
            case 0x42:   // ★ P1-10：RET u8(n) → 1 字节
                pc += 1; break;
            case 0x55:   // ★ P1-3：THROW_CONST u16(idx) → 2 字节
                pc += 2; break;
            case 0x26:   // ★ 原生库调用 NATIVE u16 + u8 + u8 → 4 字节操作数
                pc += 4; break;
            default: break;
        }
    }

    // =========================================================================
    //  主解释循环
    // =========================================================================
    void run() {
        preScanFunctions();
        pc = 0;
        execLoop(0);
    }

    // ★ ABI v4：解释器主循环抽成独立方法，供"原生库回调 Ae 函数"嵌套执行。
    //   stopDepth > 0 表示"跑到调用栈回落到 stopDepth 就返回"（回调已经返回了）。
    void execLoop(size_t stopDepth) {
        while (pc < code.size()) {
            if (stopDepth > 0 && callStack.size() <= stopDepth) return;
            instrPC = pc;                    // ★ 记录当前指令起点，出错时用它定位
            uint8_t op = code[pc++];
            try {
            switch (op) {
                case 0x00: break; // NOP

                case 0x01: { // LOAD_CONST u16(idx) —— ★ P1-2：下标校验
                    uint16_t idx = readU16(code, pc);
                    if (idx >= constPool.size())
                        throw runtime_error("常量池下标越界: #" + to_string(idx) +
                                            "（池中只有 " + to_string(constPool.size()) + " 项）");
                    const Const& c = constPool[idx];
                    if (c.tag == Const::UTF8)       push(Value(make_shared<string>(c.s)));
                    else if (c.tag == Const::INT32) push(Value((int64_t)c.i));
                    else if (c.tag == Const::INT64) push(Value((int64_t)c.i64));   // ★ P0-2
                    else if (c.tag == Const::DOUBLE) push(Value(c.d));
                    else if (c.tag == Const::NULLV)  push(Value());   // ★ null → VAL_NULL
                    break;
                }

                case 0x02: { // CALL 内置 u8(id) u8(argc) u8(want)
                    uint8_t fid  = readU8(code, pc);
                    uint8_t argc = readU8(code, pc);
                    uint8_t want = readU8(code, pc);
                    invoke(fid, argc, want);
                    break;
                }

                case 0x03: return; // HALT

                // ─── 变量 ───
                case 0x04: { // STORE u16(slot)
                    uint16_t slot = readU16(code, pc);
                    if (slot >= globals.size()) globals.resize(slot + 1);
                    globals[slot] = pop();
                    break;
                }
                case 0x05: { // LOAD_VAR u16(slot)
                    uint16_t slot = readU16(code, pc);
                    if (slot >= globals.size()) globals.resize(slot + 1);
                    push(globals[slot]);
                    break;
                }

                // ─── 算术（★ 运行期动态分派：整数/浮点自动提升）───
                //     整数族(0x06-0x09) 与 浮点族(0x0A-0x0D) 的语义现在完全一致，
                //     均按两个操作数的实际类型决定结果，编译器选哪族都不影响正确性。
                case 0x06: case 0x0A: { Value r = pop(), l = pop(); push(doAdd(l, r)); break; }
                case 0x07: case 0x0B: { Value r = pop(), l = pop(); push(doSub(l, r)); break; }
                case 0x08: case 0x0C: { Value r = pop(), l = pop(); push(doMul(l, r)); break; }
                case 0x09: case 0x0D: { Value r = pop(), l = pop(); push(doDiv(l, r)); break; }
                case 0x0E: { Value v = pop(); push(Value((double)toDouble(v))); break; } // ITOD
                case 0x0F: { Value r = pop(), l = pop(); push(doMod(l, r)); break; }

                // ─── ★ 严格比较（整数族与浮点族统一实现）───
                //     类型不同 → == / != / < / <= / > / >= 依次为 false/true/false...
                case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
                case 0x16: case 0x17: case 0x18: case 0x19: case 0x1A: case 0x1B: {
                    doCompare(op);
                    break;
                }

                // ─── ★ 布尔常量与逻辑非（P1-4）───
                case 0x1C: { push(Value(true));  break; }
                case 0x1D: { push(Value(false)); break; }
                case 0x1E: { Value v = pop(); push(Value(isFalse(v))); break; }   // NOT：返回真 bool

                // ─── ★ P2-1 位运算 / P2-2 幂 ───
                case 0x1F: {                          // BNOT
                    Value v = pop();
                    requireInt(v, "~");
                    push(Value(~v.i));
                    break;
                }
                case 0x20: case 0x21: case 0x22: {    // BAND / BOR / BXOR
                    Value r = pop(), l = pop();
                    const char* nm = (op == 0x20) ? "&" : (op == 0x21) ? "|" : "^";
                    requireInt(l, nm); requireInt(r, nm);
                    int64_t res = (op == 0x20) ? (l.i & r.i)
                                : (op == 0x21) ? (l.i | r.i)
                                :                (l.i ^ r.i);
                    push(Value(res));
                    break;
                }
                case 0x23: case 0x24: {               // SHL / SHR
                    Value r = pop(), l = pop();
                    requireInt(l, op == 0x23 ? "<<" : ">>");
                    requireInt(r, op == 0x23 ? "<<" : ">>");
                    unsigned sh = (unsigned)((uint64_t)r.i & 63u);   // 明确按 0..63 取模，避免 UB
                    if (op == 0x23) {
                        uint64_t u = ((uint64_t)l.i) << sh;
                        int64_t res; memcpy(&res, &u, 8);
                        push(Value(res));
                    } else {
                        // 算术右移（补符号位），手工实现以完全避开实现定义行为
                        int64_t res;
                        if (sh == 0) res = l.i;
                        else if (l.i >= 0) res = (int64_t)(((uint64_t)l.i) >> sh);
                        else res = (int64_t)((((uint64_t)l.i) >> sh) | (~(uint64_t)0 << (64 - sh)));
                        push(Value(res));
                    }
                    break;
                }
                case 0x25: {                          // POW
                    Value r = pop(), l = pop();
                    requireNumeric(l, "**"); requireNumeric(r, "**");
                    if (l.kind == VAL_INT && r.kind == VAL_INT && r.i >= 0 && r.i <= 63) {
                        int64_t res = 1, base = l.i;   // 整数幂（乘法回绕）
                        for (int64_t k = 0; k < r.i; k++) res = wrapMul(res, base);
                        push(Value(res));
                    } else {
                        push(Value(pow(toDouble(l), toDouble(r))));
                    }
                    break;
                }

                // ─── 控制流 ───
                case 0x2A: { // JZ
                    uint16_t u = readU16(code, pc);
                    Value v = pop();
                    if (isFalse(v)) pc = (size_t)((int64_t)pc + (int16_t)u);
                    break;
                }
                case 0x2B: { // JMP
                    uint16_t u = readU16(code, pc);
                    pc = (size_t)((int64_t)pc + (int16_t)u);
                    break;
                }

                // ─── 局部变量 ───
                case 0x30: { // LOCAL_STORE u16
                    uint16_t slot = readU16(code, pc);
                    if (!currentLocals) throw runtime_error("LOCAL_STORE 在函数外无效");
                    if (slot >= currentLocals->size()) currentLocals->resize(slot + 1);
                    (*currentLocals)[slot] = pop();
                    break;
                }
                case 0x31: { // LOCAL_LOAD u16
                    uint16_t slot = readU16(code, pc);
                    if (!currentLocals) throw runtime_error("LOCAL_LOAD 在函数外无效");
                    if (slot >= currentLocals->size()) currentLocals->resize(slot + 1);
                    push((*currentLocals)[slot]);
                    break;
                }

                // ─── 通用表（数组 / 字典合一）───
                case 0x50: { // NEW_TABLE u16(count) —— 创建空表并压栈；count 仅作容量预留
                    uint16_t count = readU16(code, pc);
                    shared_ptr<Table> tab = make_shared<Table>();
                    if (count > 0) tab->arr.reserve(count);
                    push(Value::makeTable(tab));
                    break;
                }
                case 0x51: { // TABLE_GET：栈 [tab, key] → value（未定义返回 null）
                    Value keyV = pop(), tabV = pop();
                    // ★ P2-11：字符串支持 s[i]（1-based，按码点），越界返回 null
                    if (tabV.kind == VAL_STR) {
                        if (keyV.kind != VAL_INT)
                            throw runtime_error(string("字符串索引必须是 int（字符序号，1-based），得到 ") +
                                                typeName(keyV.kind));
                        string ch = utf8CharAt(tabV.str ? *tabV.str : string(), keyV.i);
                        if (ch.empty() && keyV.i > 0) push(Value());          // 越界 → null
                        else push(Value(ch));
                        break;
                    }
                    if (!tabV.isTable()) throw runtime_error(std::string("非表类型：'") + typeName(tabV.kind) +
                        "' 不能取键/字段（. 或 [] 只能用在表上）");
                    Value out;
                    if (!tabV.tab->get(keyV, out)) out = Value();   // null
                    push(out);
                    break;
                }
                case 0x52: { // TABLE_SET：栈 [tab, key, value] → 就地修改，结果（表）压回栈顶
                    Value val = pop(), keyV = pop(), tabV = pop();
                    if (!tabV.isTable()) throw runtime_error(std::string("非表类型：'") + typeName(tabV.kind) +
                        "' 不能按 key 赋值（t[k] = v 里的 t 必须是表）");
                    tabV.tab->set(keyV, val);
                    push(tabV);   // 表回到栈顶
                    break;
                }

                // ★ P1-3：取表的所有键（顺序确定：先数组段 1..n，再哈希段按键排序）
                //   值为 null 的槽不出现（与 Lua pairs 跳过 nil 一致）
                case 0x53: {
                    Value v = pop();
                    if (!v.isTable())
                        throw runtime_error(string("for-in 需要一个表，得到 ") + typeName(v.kind));
                    shared_ptr<Table> out = make_shared<Table>();
                    const Table* t = v.tab.get();
                    for (size_t i = 0; i < t->arr.size(); i++)
                        if (t->arr[i].kind != VAL_NULL)
                            out->arr.push_back(Value((int64_t)(i + 1)));
                    for (const auto& kv : t->mapv) {
                        if (kv.second.kind == VAL_NULL) continue;
                        switch (kv.first.kind) {
                            case VAL_INT:    out->arr.push_back(Value((int64_t)kv.first.i)); break;
                            case VAL_BOOL:   out->arr.push_back(Value(kv.first.i != 0)); break;
                            case VAL_DOUBLE: out->arr.push_back(Value(kv.first.f)); break;
                            case VAL_STR:    out->arr.push_back(Value(kv.first.str ? *kv.first.str : string())); break;
                            default: break;
                        }
                    }
                    push(Value::makeTable(out));
                    break;
                }
                case 0x54: { // TABLE_LEN
                    Value v = pop();
                    push(Value(v.isTable() ? (int64_t)v.tab->length() : (int64_t)0));
                    break;
                }
                case 0x55: { // THROW_CONST u16(idx)：抛出常量字符串作为运行错误
                    uint16_t idx = readU16(code, pc);
                    if (idx >= constPool.size()) throw runtime_error("THROW_CONST 常量下标越界");
                    throw runtime_error(constPool[idx].s);
                }
                case 0x27: { // ★ PUSH_HANDLER u16(相对偏移)：注册错误处理器
                    uint16_t rel = readU16(code, pc);
                    Handler h;
                    h.frameDepth = callStack.size();
                    h.stackSize  = stack.size();
                    h.landing    = (size_t)((int64_t)pc + (int16_t)rel);
                    handlers.push_back(h);
                    break;
                }
                case 0x28: { // ★ POP_HANDLER：注销最近注册的处理器
                    if (!handlers.empty()) handlers.pop_back();
                    break;
                }
                case 0x26: { // NATIVE u16(常量名 "mod.func") u8(argc) u8(want)
                    uint16_t idx = readU16(code, pc);
                    uint8_t  argc = readU8(code, pc);
                    uint8_t  want = readU8(code, pc);
                    if (idx >= constPool.size())
                        throw runtime_error("NATIVE 常量下标越界: #" + to_string(idx));
                    const string& key = constPool[idx].s;
                    auto it = nativeFns.find(key);
                    if (it == nativeFns.end())
                        throw runtime_error("原生函数未加载: " + key +
                            "（请在 .ae 文件里写 imp <库名> 后重新编译）");
                    const AeNativeDef* d = it->second;
                    if (d->minArgs >= 0 && (int)argc < d->minArgs)
                        throw runtime_error(key + " 至少需要 " + to_string(d->minArgs) + " 个参数");
                    if (d->maxArgs >= 0 && (int)argc > d->maxArgs)
                        throw runtime_error(key + " 最多接受 " + to_string(d->maxArgs) + " 个参数");
                    vector<Value> args(argc);
                    for (uint8_t i = 0; i < argc; i++) args[argc - 1 - i] = pop();
                    vector<AeValue> av(argc);
                    for (uint8_t i = 0; i < argc; i++) av[i] = toAeValue(args[i]);
                    nativeArgs = args;      // ★ ABI v4：见成员说明（函数值句柄靠它找回本体）

                    nativePoolStr.clear();
                    nativePoolTab.clear();
                    pendingNativeError.clear();
                    size_t before = stack.size();
                    int pushed = d->fn(reinterpret_cast<AeVM*>(this), (int)argc, argc ? av.data() : nullptr);
                    if (!pendingNativeError.empty()) {
                        string e = pendingNativeError;
                        pendingNativeError.clear();
                        nativePoolStr.clear(); nativePoolTab.clear();
                        thrownCode = AE_E_NATIVE;
                        throw runtime_error(e);
                    }
                    size_t produced = (pushed > 0) ? (size_t)pushed : 0;
                    size_t actual = stack.size() - before;
                    if (produced > actual) produced = actual;   // 防御：库虚报个数
                    adjustTop(produced, want);
                    nativePoolStr.clear();
                    nativePoolTab.clear();
                    break;
                }

                // ─── 函数定义 / 调用 / 返回 ───
                case 0x40: {
                    FuncDef fd;
                    readFuncHeader(code, pc, fd, bcMinor);
                    if (fd.funcId >= funcs.size()) funcs.resize(fd.funcId + 1);
                    funcs[fd.funcId] = fd;
                    pc += fd.codeSize;
                    break;
                }
                case 0x41: { // CALL_FUNC u16(id) u8(argc) u8(want)
                    uint16_t funcId = readU16(code, pc);
                    uint8_t  argc   = readU8(code, pc);
                    uint8_t  want   = readU8(code, pc);
                    callUserFunction(funcId, argc, want);
                    break;
                }
                case 0x42: { // ★ P1-10：RET u8(n) —— 把栈顶 n 个值作为返回值，
                             //   并彻底丢弃本帧遗留的所有操作数（回到 stackBase）
                    uint8_t n = readU8(code, pc);
                    if (callStack.empty()) return;
                    // ★ 必须 move：否则每次返回都整份拷贝帧（含 locals 向量）→
                    //   等于每次调用多一次 malloc+拷贝+free。实测这是 fib 的最大热点之一。
                    CallFrame fr = std::move(callStack.back());
                    callStack.pop_back();
                    // ★ 闭包：帧要没了，先把本帧打开的上值都关掉（值搬进堆上的闭包），
                    //   否则闭包会指向已释放的 locals —— 这是闭包逃逸时最容易出的悬空 bug
                    fr.closeUpvalues();
                    // ★ 兜底：清掉属于已返回帧的处理器（正常路径由编译器补 POP_HANDLER）
                    while (!handlers.empty() && handlers.back().frameDepth > callStack.size())
                        handlers.pop_back();

                    vector<Value> rets;
                    for (uint8_t i = 0; i < n; i++) rets.push_back(pop());
                    std::reverse(rets.begin(), rets.end());

                    stack.resize(fr.stackBase);          // 清掉函数内的残留
                    for (const Value& v : rets) stack.push_back(v);
                    adjustTop(rets.size(), fr.want);     // 按调用方要求裁剪/补齐

                    currentLocals = callStack.empty() ? nullptr : &(callStack.back().locals);
                    pc = fr.returnPC;
                    break;
                }
                case 0x43: { // POP
                    (void)pop();
                    break;
                }
                // ─── ★ 1.7：闭包 ───
                case 0x44: { // CLOSURE u16(funcId) u8(nups)：按原型的上值表抓当前环境的变量
                    uint16_t funcId = readU16(code, pc);
                    uint8_t  nups   = readU8(code, pc);
                    if (funcId >= funcs.size())
                        throw runtime_error("CLOSURE 引用了未定义的函数 id=" + to_string(funcId));
                    const FuncDef& fd = funcs[funcId];
                    if (fd.ups.size() != nups)
                        throw runtime_error("CLOSURE 的上值个数与原型不符（字节码内部不一致）");
                    auto cl = make_shared<Closure>();
                    cl->funcId = funcId;
                    for (const UpvalDesc& d : fd.ups) {
                        if (d.fromParentLocal) {
                            cl->ups.push_back(makeOpenUpvalue(d.index));
                        } else {
                            if (callStack.empty() || !callStack.back().closure ||
                                d.index >= callStack.back().closure->ups.size())
                                throw runtime_error("CLOSURE 继承上值时找不到父闭包（字节码内部不一致）");
                            cl->ups.push_back(callStack.back().closure->ups[d.index]);
                        }
                    }
                    push(Value::makeFunc(cl));
                    break;
                }
                case 0x45: { // GET_UPVAL u16(i)
                    uint16_t i = readU16(code, pc);
                    if (callStack.empty() || !callStack.back().closure ||
                        i >= callStack.back().closure->ups.size())
                        throw runtime_error("GET_UPVAL：当前帧没有这个上值（字节码内部不一致）");
                    push(callStack.back().closure->ups[i]->get());
                    break;
                }
                case 0x46: { // SET_UPVAL u16(i)
                    uint16_t i = readU16(code, pc);
                    if (callStack.empty() || !callStack.back().closure ||
                        i >= callStack.back().closure->ups.size())
                        throw runtime_error("SET_UPVAL：当前帧没有这个上值（字节码内部不一致）");
                    callStack.back().closure->ups[i]->get() = pop();
                    break;
                }
                case 0x47: { // CLOSE_SLOT u16(slot)：把一个局部槽上的上值"关掉"
                             //   循环每轮末尾发这条，下一轮的闭包就会拿到【全新的】变量
                    uint16_t slot = readU16(code, pc);
                    if (!callStack.empty()) {
                        CallFrame& fr = callStack.back();
                        for (size_t k = 0; k < fr.openUps.size(); k++) {
                            if (fr.openUps[k].first == slot) {
                                fr.openUps[k].second->close();
                                fr.openUps.erase(fr.openUps.begin() + (long)k);
                                break;
                            }
                        }
                    }
                    break;
                }
                case 0x48: { // CALL_VALUE u8(argc) u8(want)：栈上是 [ 函数值 参数... ]
                    uint8_t argc = readU8(code, pc);
                    uint8_t want = readU8(code, pc);
                    vector<Value> args(argc);
                    for (int i = argc - 1; i >= 0; i--) args[i] = pop();
                    Value callee = pop();
                    if (!callee.isFunc() || !callee.fn)
                        throw runtime_error("试图调用一个不是函数的值（type = " +
                                            string(typeName(callee.kind)) + "）");
                    if (callStack.size() >= maxCallDepth)
                        throw runtime_error("调用栈溢出（超过 " + to_string(maxCallDepth) +
                                            " 层，可用 --max-depth 调整）");
                    // 参数先压回栈（callUserFunction 自己从栈上取），再真正调用
                    for (const Value& a : args) push(a);
                    callUserFunction(callee.fn->funcId, argc, want, callee.fn);
                    break;
                }

                default:
                    throw runtime_error("未知指令: 0x" + to_string(op));
            }
            } catch (const ExitException&) {
                throw;                                   // ★ exit() 不可捕获
            } catch (const exception& e) {
                if (!handleRuntimeError(e.what())) throw; // 没处理器 → 继续往外抛
            }
        }
    }

    // ★ 构造脚本可见的错误表：{code, kind, msg, file, line, col, stack}
    Value buildErrorTable(const string& msg) {
        shared_ptr<Table> t = make_shared<Table>();
        const char* code = thrownCode.empty() ? classifyRun(msg) : thrownCode.c_str();
        int line = 0, col = 1, fi = 0;
        posForPC(instrPC, line, col, &fi);
        // ★ 列号换成【显示列】（中文算 2 列），跟打印出来的 ^ 位置一致
        if (line > 0) {
            std::string raw = aediag::lineAt(sourceTextOf(fi), line);
            if (!raw.empty()) col = aediag::displayCol(raw, col, nullptr) + 1;
        }
        t->set(Value(string("code")), Value(string(code)));
        t->set(Value(string("kind")), Value(string(kindForCode(code))));
        t->set(Value(string("msg")),  Value(msg));
        t->set(Value(string("file")), Value(fileNameOf(fi)));
        t->set(Value(string("line")), Value((int64_t)line));
        t->set(Value(string("col")),  Value((int64_t)col));
        t->set(Value(string("stack")), Value(stackTrace()));
        thrownCode.clear();
        return Value::makeTable(t);
    }
    // ★ 出错时回退到最近的处理器；没有处理器就继续往外抛
    bool handleRuntimeError(const string& msg) {
        if (handlers.empty()) return false;
        Value err = buildErrorTable(msg);            // 先用【出错现场】的信息建表
        Handler h = handlers.back();
        handlers.pop_back();
        while (callStack.size() > h.frameDepth) {
            callStack.back().closeUpvalues();        // ★ 闭包：退掉的帧也要关上值
            callStack.pop_back();
        }
        currentLocals = callStack.empty() ? nullptr : &(callStack.back().locals);
        stack.resize(h.stackSize);
        push(err);                                   // catch 变量就是它
        pc = h.landing;
        return true;
    }

    // ★ 1.7：取当前帧某个局部槽的"开放上值"。
    //   同一个槽被抓多次 → 共享同一个 Upvalue（所以两个闭包改的是同一个变量）。
    shared_ptr<Upvalue> makeOpenUpvalue(uint16_t slot) {
        if (callStack.empty()) throw runtime_error("没有活动帧，无法捕获变量");
        CallFrame& fr = callStack.back();
        if (slot >= fr.locals.size())
            throw runtime_error("捕获了不存在的局部槽 " + to_string(slot) + "（字节码内部不一致）");
        for (auto& kv : fr.openUps)
            if (kv.first == slot) return kv.second;
        auto up = make_shared<Upvalue>();
        // 注意：指向的是 locals 这块【堆缓冲区】里的元素，不是 CallFrame 对象本身。
        // vector<CallFrame> 扩容会搬 CallFrame，但搬不动 locals 的缓冲区，所以指针依旧有效。
        up->open = &fr.locals[slot];
        fr.openUps.push_back({slot, up});
        return up;
    }

    // ★ 1.7：cl 非空 = 调用一个闭包（帧里带上上值表）；空 = 普通全局函数
    // ★ ABI v4：从原生库回调一个 Ae 函数值。
    //   这是一个"嵌套执行"：外层解释器状态全部保存/恢复，回调内部可以
    //   继续调用原生库、可以用 try/catch、可以 raise；出错则返回 false 并把
    //   原因写进 err（不在这里决定是抛还是忽略 —— 交给库，也交给外层的 try/catch）。
    bool callValueFromNative(const Value& fn, const vector<Value>& args, Value* out, string* err) {
        if (!fn.isFunc() || !fn.fn) { if (err) *err = "call_value: 不是函数值"; return false; }
        if (args.size() > 255)      { if (err) *err = "call_value: 参数太多"; return false; }

        size_t savedPC = pc, savedInstrPC = instrPC;
        // (原来的 currentLocals 保存指针已改成按帧深度重取，见下方恢复处)
        size_t savedDepth = callStack.size();
        size_t savedStackTop = stack.size();
        string savedPending = pendingNativeError;
        vector<Handler> savedHandlers;               // 回调有【自己的】try 层
        savedHandlers.swap(handlers);
        bool ok = true;

        try {
            for (const Value& a : args) push(a);
            callUserFunction(fn.fn->funcId, (uint8_t)args.size(), 1, fn.fn);
            execLoop(savedDepth);
            Value r;
            if (stack.size() > savedStackTop) r = pop();
            if (out) *out = r;
        } catch (const ExitException&) {
            // exit() 不能被回调吞掉：恢复状态后一路抛出去，让程序真的结束
            while (callStack.size() > savedDepth) { callStack.back().closeUpvalues(); callStack.pop_back(); }
            stack.resize(savedStackTop);
            handlers.swap(savedHandlers);
            pc = savedPC; instrPC = savedInstrPC;
            // ★ 不能直接恢复保存的【指针】：嵌套调用会让 callStack 扩容/搬迁，
            //   那个指针就悬空了（症状：回调返回后宿主函数的局部变量全被写进
            //   已释放内存，变量莫名变成 null/0）。按帧深度重新取。
            currentLocals = (savedDepth == 0) ? nullptr : &(callStack[savedDepth - 1].locals);
            pendingNativeError = savedPending;
            throw;
        } catch (const exception& e) {
            if (err) *err = e.what();
            ok = false;
        }
        while (callStack.size() > savedDepth) { callStack.back().closeUpvalues(); callStack.pop_back(); }
        stack.resize(savedStackTop);
        handlers.swap(savedHandlers);
        pc = savedPC; instrPC = savedInstrPC;
        currentLocals = (savedDepth == 0) ? nullptr : &(callStack[savedDepth - 1].locals);   // ★ 见上
        pendingNativeError = savedPending;
        return ok;
    }

    void callUserFunction(uint16_t funcId, uint8_t argc, uint8_t want,
                          shared_ptr<Closure> cl = nullptr) {
        if (funcId >= funcs.size()) throw runtime_error("调用未定义的函数 id=" + to_string(funcId));
        const FuncDef& fd = funcs[funcId];
        if (argc != fd.paramCount)
            throw runtime_error("函数参数数量不匹配: 期望 " + to_string(fd.paramCount) +
                                "，实际传入 " + to_string(argc));
        if (callStack.size() >= maxCallDepth)
            throw runtime_error("调用栈溢出（超过 " + to_string(maxCallDepth) +
                                " 层，可用 --max-depth 调整）");
        vector<Value> args(argc);
        for (int i = argc - 1; i >= 0; i--) args[i] = pop();

        CallFrame frame;
        frame.funcId    = funcId;
        frame.returnPC  = pc;
        frame.stackBase = stack.size();      // ★ P1-10：本帧的操作数栈基址
        frame.want      = want;              // ★ P1-5
        frame.closure   = std::move(cl);
        frame.locals.assign(fd.localCount, Value());
        for (uint16_t i = 0; i < argc && i < fd.paramCount; i++) frame.locals[i] = args[i];

        callStack.push_back(std::move(frame));
        currentLocals = &(callStack.back().locals);
        pc = fd.entryPC;
    }

    // =========================================================================
    //  .aeo 加载
    // =========================================================================
    // 读一个小节：u32 长度 + 字节
    static void readBlob(const vector<uint8_t>& b, size_t& p, string& out, const char* what) {
        if (p + 4 > b.size()) throw runtime_error(string(what) + " 长度字段截断");
        uint32_t n = readU32(b, p);
        if (n > b.size() - p) throw runtime_error(string(what) + " 数据越界: " + to_string(n));
        out.assign((const char*)(b.data() + p), n);
        p += n;
    }

    void load(const string& path) {
        ifstream f(path, ios::binary);
        if (!f) throw LoadIOError("无法打开: " + path + "（源文件要先编译: aec 源文件.ae）");
        vector<uint8_t> all((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
        if (all.size() < 16) throw runtime_error("文件过短，不是有效的 .aeo 文件");

        size_t p = 0;
        if (all[p]!='A' || all[p+1]!='e' || all[p+2]!='B' || all[p+3]!='c')
            throw runtime_error("魔数不匹配");
        p += 4;
        // ★ P1-2：版本必须校验（此前只是 `p += 2` 跳过，改成 99.42 也照跑）
        uint8_t major = readU8(all, p);
        uint8_t minor = readU8(all, p);
        if (major != 1)
            throw runtime_error("不支持的字节码主版本: " + to_string(major) + "（本 VM 支持 1.x）");
        if (minor > 7)
            throw runtime_error("字节码次版本过新: 1." + to_string(minor) + "（本 VM 支持到 1.7，请重新编译源码）");
        bcMinor = minor;      // ★ 1.7 起 FUNC_DEF 头带上值描述表（闭包）
        (void)readU16(all, p); // flags

        uint32_t poolCount = readU32(all, p);
        if (poolCount > 1024u * 1024u) throw runtime_error("常量池过大: " + to_string(poolCount));
        constPool.resize(poolCount);
        for (uint32_t i = 0; i < poolCount; i++) {
            if (p >= all.size()) throw runtime_error("常量池截断（条目 #" + to_string(i) + "）");
            uint8_t tag = readU8(all, p);
            if (tag != Const::UTF8 && tag != Const::INT32 && tag != Const::DOUBLE &&
                tag != Const::NULLV && tag != Const::INT64)
                throw runtime_error("未知的常量池 tag: 0x" + to_string(tag) + "（条目 #" + to_string(i) + "）");
            constPool[i].tag = (decltype(Const::tag))tag;
            if (tag == Const::NULLV) {
                // ★ null 字面量：仅 tag 字节，无附加数据
                constPool[i].s = string();   // 保持聚合初始化等价语义
            } else if (tag == Const::UTF8) {
                if (p + 4 > all.size()) throw runtime_error("常量池 UTF-8 长度截断");
                uint32_t len = readU32(all, p);
                if (len > all.size() - p) throw runtime_error("常量池 UTF-8 数据截断（len=" + to_string(len) + "）");
                constPool[i].s = string((const char*)(all.data() + p), len);
                p += len;
            } else if (tag == Const::INT32) {
                if (p + 4 > all.size()) throw runtime_error("常量池 int32 截断");
                constPool[i].i = (int32_t)readU32(all, p);
            } else if (tag == Const::INT64) {
                // ★ P0-2：8 字节大端
                if (p + 8 > all.size()) throw runtime_error("常量池 int64 截断");
                uint64_t bits = 0;
                for (int s = 56; s >= 0; s -= 8) bits = (bits << 8) | readU8(all, p);
                memcpy(&constPool[i].i64, &bits, 8);   // 位精确，避免实现定义的有符号转换
            } else if (tag == Const::DOUBLE) {
                if (p + 8 > all.size()) throw runtime_error("常量池 double 截断");
                uint64_t bits = 0;
                for (int s = 56; s >= 0; s -= 8) bits = (bits << 8) | readU8(all, p);
                memcpy(&constPool[i].d, &bits, 8);
            }
        }
        // ★ P1-1/P1-2：v1.1 新增的调试信息段（次版本 0 的旧文件没有这些段）
        if (minor >= 1) {
            readBlob(all, p, srcName, "源文件名");
            if (minor >= 6) {                                  // ★ 源文件表
                if (p + 4 > all.size()) throw runtime_error("源文件表长度截断");
                uint32_t fc = readU32(all, p);
                if (fc > 4096) throw runtime_error("源文件表过大: " + to_string(fc));
                for (uint32_t i = 0; i < fc; i++) {
                    string fnm;
                    readBlob(all, p, fnm, "源文件名");
                    fileNames.push_back(fnm);
                }
            }
            if (fileNames.empty()) fileNames.push_back(srcName);
            if (p + 4 > all.size()) throw runtime_error("函数名表长度截断");
            uint32_t nameCount = readU32(all, p);
            if (nameCount > 65536u) throw runtime_error("函数名表过大: " + to_string(nameCount));
            for (uint32_t i = 0; i < nameCount; i++) {
                if (p + 2 > all.size()) throw runtime_error("函数名表截断");
                uint16_t fid = readU16(all, p);
                string nm;
                readBlob(all, p, nm, "函数名");
                funcNames.push_back({fid, nm});
            }
            if (p + 4 > all.size()) throw runtime_error("行号表长度截断");
            uint32_t lineCount = readU32(all, p);
            if (lineCount > 64u * 1024u * 1024u / 8u) throw runtime_error("行号表过大: " + to_string(lineCount));
            // 1.1~1.3 每项 8 字节；1.4+ 12 字节（列号）；1.6+ 14 字节（文件索引）
            size_t entrySize = (minor >= 6) ? 14u : (minor >= 4) ? 12u : 8u;
            for (uint32_t i = 0; i < lineCount; i++) {
                if (p + entrySize > all.size()) throw runtime_error("行号表截断");
                uint32_t off = readU32(all, p);
                uint32_t ln  = readU32(all, p);
                uint32_t cl  = 1;
                uint16_t fi  = 0;
                if (entrySize >= 12) cl = readU32(all, p);
                if (entrySize >= 14) fi = readU16(all, p);
                lineTable.push_back({off, ln, cl, fi});
            }
            sort(lineTable.begin(), lineTable.end(),
                 [](const LineEntry& a, const LineEntry& b) { return a.off < b.off; });
        }

        if (p + 4 > all.size()) throw runtime_error("codeLength 字段截断");
        uint32_t codeLen = readU32(all, p);
        if (codeLen > all.size() - p) throw runtime_error("codeLength 越界: " + to_string(codeLen));
        if (codeLen > 16u * 1024u * 1024u) throw runtime_error("codeLength 过大: " + to_string(codeLen));
        code.assign(all.begin() + p, all.begin() + p + codeLen);

        // ★ 1.3：代码段之后的原生库导入表
        scriptDir = aehost::dirOf(path);
        srcPath   = path;
        if (minor >= 3 && p + codeLen + 4 <= all.size()) {
            size_t q = p + codeLen;
            uint32_t modCount = readU32(all, q);
            if (modCount <= 1024) {
                vector<string> modNames;
                for (uint32_t i = 0; i < modCount; i++) {
                    if (q + 4 > all.size()) throw runtime_error("导入表截断");
                    uint32_t n = readU32(all, q);
                    if (q + n > all.size()) throw runtime_error("导入表数据越界");
                    modNames.push_back(string((const char*)(all.data() + q), n));
                    q += n;
                }
                loadNativeModules(modNames);       // 缺库在这里就报错（跑之前）
            }
        }

        // ★ P1-2：预扫描后的函数表必须自洽（每个函数体都要在代码段内）
        for (size_t i = 0; i < funcs.size(); i++) {
            const FuncDef& fd = funcs[i];
            if (fd.codeSize == 0 && fd.entryPC == 0) continue;      // 未定义的 id 空洞
            if (fd.entryPC > code.size() || fd.codeSize > code.size() - fd.entryPC)
                throw runtime_error("字节码损坏：函数 #" + to_string(i) + " 的代码范围越界");
        }
        if (code.empty()) throw runtime_error("字节码损坏：代码段为空");
    }
};

static const char* classifyRun(const std::string& m);
static const char* kindForCode(const char* code);
static std::string helpForRun(const std::string& m);
// =============================================================================
//  ★ 运行期诊断渲染（分类表集中维护）
// =============================================================================
static const char* classifyRunImpl(const std::string& m) {
    struct Rule { const char* frag; const char* code; };
    static const Rule kRules[] = {
        { "不能直接比较",       AE_E_TYPE },
        { "大小比较要求",       AE_E_TYPE },
        { "不支持",             AE_E_TYPE },
        { "只支持 int",         AE_E_TYPE },
        { "除以零",             AE_E_DIV_ZERO },
        { "字符串索引",         AE_E_INDEX },
        { "表键",               AE_E_INDEX },
        { "表索引",             AE_E_INDEX },
        { "非表类型",           AE_E_INDEX },
        { "越界",               AE_E_BC_CORRUPT },
        { "字节码",             AE_E_BC_CORRUPT },
        // ★ 坏 .aeo / 版本不对也归到「字节码损坏」，别报成 E2011 内部错误
        { "魔数",               AE_E_BC_CORRUPT },
        { "文件过短",           AE_E_BC_CORRUPT },
        { "版本",               AE_E_BC_CORRUPT },
        { "截断",               AE_E_BC_CORRUPT },
        { "常量池",             AE_E_BC_CORRUPT },
        { "导入表",             AE_E_BC_CORRUPT },
        { "代码段为空",         AE_E_BC_CORRUPT },
        { "未定义的函数 id",    AE_E_BC_FUNC },
        { "调用未定义的函数",   AE_E_BC_FUNC },
        { "原生函数未加载",     AE_E_NATIVE },
        { "原生库",             AE_E_NATIVE },
        { "调用栈溢出",         AE_E_STACK_OVER },
        { "assert",             AE_E_ASSERT },
        { "函数参数数量不匹配", AE_E_ARITY },
        { "读取输入",           AE_E_INPUT },
        // ★ 内建函数的参数类型/解析失败（以前落到 E2011「VM 内部错误」，把用户的错说成
        //   虚拟机的错）：asStr/asInt/asTable 与 int()/float() 的解析
        { "需要字符串参数",     AE_E_TYPE },
        { "需要 int 参数",      AE_E_TYPE },
        { "需要表参数",         AE_E_TYPE },
        { "解析为整数",         AE_E_TYPE },
        { "解析为浮点数",       AE_E_TYPE },
        { "个参数，得到",       AE_E_ARITY },
    };
    for (const Rule& r : kRules)
        if (m.find(r.frag) != std::string::npos) return r.code;
    return AE_E_INTERNAL;
}
static const char* classifyRun(const std::string& m) { return classifyRunImpl(m); }
static const char* kindForCode(const char* code) {
    struct K { const char* code; const char* kind; };
    static const K kKinds[] = {
        { AE_E_TYPE, "type" }, { AE_E_DIV_ZERO, "divzero" }, { AE_E_INDEX, "index" },
        { AE_E_BC_FUNC, "bytecode" }, { AE_E_BC_CORRUPT, "bytecode" },
        { AE_E_STACK_OVER, "stackoverflow" }, { AE_E_ASSERT, "assert" },
        { AE_E_USER_ERROR, "user" }, { AE_E_NATIVE, "native" },
        { AE_E_ARITY, "arity" }, { AE_E_INPUT, "input" },
    };
    for (const K& k : kKinds) if (std::strcmp(k.code, code) == 0) return k.kind;
    return "internal";
}
static std::string helpForRun(const std::string& m) {
    if (m.find("不能直接比较") != std::string::npos || m.find("大小比较要求") != std::string::npos)
        return "两侧类型要一致：写 float(x) 或把字面量写成 1.0。";
    if (m.find("不支持 bool") != std::string::npos)
        return "bool 不参与算术，需要时先 int(x) 显式转换。";
    if (m.find("原生函数未加载") != std::string::npos)
        return "请在 .ae 文件里写 imp <库名>，然后重新编译。";
    return std::string();
}
// ★ codeHint：抛出时已经定好的错误码（原生库 / assert / error / raise 表）
//   以前这里只用消息去猜，猜不中一律印成 E2011「VM 内部错误」——库参数写错、脚本
//   自己 raise 的错误也被说成内部错误，而 try/catch 里看到的却是真实码，两处对不上。
static void renderRunError(VM& vm, const std::string& what, const char* codeHint = nullptr) {
    aediag::AeDiag d;
    d.code = (codeHint && *codeHint) ? codeHint : classifyRun(what);
    d.msg  = what;
    int line = 0, col = 1, fi = 0;
    vm.posForPC(vm.instrPC, line, col, &fi);
    if (line > 0) { d.span = { vm.fileNameOf(fi), line, col, 1 }; d.hasSpan = true; }
    std::string h = helpForRun(what);
    if (!h.empty()) d.helps.push_back(h);
    std::string trace = vm.stackTrace();
    size_t p = 0;
    while (p < trace.size()) {
        size_t q = trace.find('\n', p);
        if (q == std::string::npos) break;
        d.stackLines.push_back(trace.substr(p, q - p));
        p = q + 1;
    }
    aediag::printDiag(d, vm.sourceTextOf(fi));
}

// =============================================================================
//  ★ 原生库 ABI 实现（宿主侧）：把函数表注入给库
// =============================================================================
static VM* asVM(AeVM* v) { return reinterpret_cast<VM*>(v); }

static void api_push_null(AeVM* v)                     { asVM(v)->push(Value()); }
static void api_push_int(AeVM* v, int64_t x)           { asVM(v)->push(Value(x)); }
static void api_push_float(AeVM* v, double x)          { asVM(v)->push(Value(x)); }
static void api_push_bool(AeVM* v, int b)              { asVM(v)->push(Value(b != 0)); }
static void api_push_str(AeVM* v, const char* s, size_t n) {
    asVM(v)->push(Value(make_shared<string>(s ? string(s, n) : string())));
}
static void api_push_table(AeVM* v, AeValue t) {
    VM* vm = asVM(v);
    Table* tp = vm->findPoolTable(t);
    if (!tp) { vm->pendingNativeError = "原生库 push 了非法的表引用"; return; }
    for (const auto& sp : vm->nativePoolTab)
        if (sp.get() == tp) { vm->push(Value::makeTable(sp)); return; }
}
static AeValue api_make_str(AeVM* v, const char* s, size_t n) {
    VM* vm = asVM(v);
    shared_ptr<string> sp = make_shared<string>(s ? string(s, n) : string());
    vm->nativePoolStr.push_back(sp);
    AeValue a; a.kind = AE_STRING; a.i = 0; a.f = 0; a.p = sp.get();
    return a;
}
static AeValue api_new_table(AeVM* v) {
    VM* vm = asVM(v);
    shared_ptr<Table> t = make_shared<Table>();
    vm->nativePoolTab.push_back(t);
    AeValue a; a.kind = AE_TABLE; a.i = 0; a.f = 0; a.p = t.get();
    return a;
}
static void api_table_set(AeVM* v, AeValue t, AeValue k, AeValue val) {
    VM* vm = asVM(v);
    Table* tp = vm->findPoolTable(t);
    if (!tp) { vm->pendingNativeError = "原生库往非法的表里写值"; return; }
    try {
        tp->set(vm->fromAeValue(k), vm->fromAeValue(val));
    } catch (const exception& e) {
        vm->pendingNativeError = e.what();
    }
}
static void api_set_error(const char* m) { g_nativeLastError = m ? m : ""; }
static void api_raise(AeVM* v, const char* m) {
    if (v) asVM(v)->pendingNativeError = m ? m : "原生库报错";
    else   g_nativeLastError = m ? m : "";
}
// ★ ABI v2：脚本命令行参数与脚本路径
static int api_arg_count(AeVM* v) { return (int)asVM(v)->scriptArgs.size(); }
static const char* api_arg_get(AeVM* v, int idx, size_t* len) {
    VM* vm = asVM(v);
    if (idx < 0 || (size_t)idx >= vm->scriptArgs.size()) { if (len) *len = 0; return ""; }
    if (len) *len = vm->scriptArgs[(size_t)idx].size();
    return vm->scriptArgs[(size_t)idx].data();
}
static const char* api_script_path(AeVM* v, size_t* len) {
    VM* vm = asVM(v);
    if (len) *len = vm->srcPath.size();
    return vm->srcPath.data();
}

// ★ ABI v3：读表。脚本传进来的表就是 VM 里的 Table（argv 在调用期间一直活着），
//   所以直接把 p 当 Table* 用；取到的值【借用】返回，不做拷贝。
// ★ ABI v4：库持有闭包句柄 + 从原生库调用 Ae 函数
static int api_func_retain(AeVM* v, AeValue fn) {
    VM* vm = asVM(v);
    if (fn.kind != AE_FUNC || !fn.p) return 0;
    const Closure* want = (const Closure*)fn.p;
    // AeValue 里的 p 是裸指针，要恢复成 shared_ptr 得从"还活着的地方"找回本体。
    // 值一定来自脚本，所以扫一遍操作数栈 / 各帧局部 / 全局就够了。
    auto match = [&](const Value& val) { return val.kind == VAL_FUNC && val.fn && val.fn.get() == want; };
    // ① 本次原生调用的实参（最常见：ui.on(w, "click", func(e){...}) 的第三个参数）
    for (const Value& val : vm->nativeArgs) if (match(val)) { vm->funcHandles.push_back(val.fn); return (int)vm->funcHandles.size(); }
    // ② 兜底：操作数栈 / 各帧局部 / 全局（值可能来自某处引用）
    for (const Value& val : vm->stack)     if (match(val)) { vm->funcHandles.push_back(val.fn); return (int)vm->funcHandles.size(); }
    for (const CallFrame& fr : vm->callStack)
        for (const Value& val : fr.locals) if (match(val)) { vm->funcHandles.push_back(val.fn); return (int)vm->funcHandles.size(); }
    for (const Value& val : vm->globals)   if (match(val)) { vm->funcHandles.push_back(val.fn); return (int)vm->funcHandles.size(); }
    return 0;
}
static void api_func_release(AeVM* v, int h) {
    VM* vm = asVM(v);
    if (h > 0 && (size_t)h <= vm->funcHandles.size()) vm->funcHandles[(size_t)h - 1].reset();
}
static int api_func_call(AeVM* v, int h, int argc, const AeValue* argv, AeValue* out, const char** err) {
    VM* vm = asVM(v);
    if (err) *err = nullptr;
    if (out) { out->kind = AE_NULL; out->i = 0; out->f = 0; out->p = nullptr; }
    if (h <= 0 || (size_t)h > vm->funcHandles.size() || !vm->funcHandles[(size_t)h - 1]) {
        vm->nativeCallError = "func_call: 句柄无效（可能已经 release 过了）";
        if (err) *err = vm->nativeCallError.c_str();
        return 0;
    }
    vector<Value> args;
    for (int i = 0; i < argc; i++) {
        try { args.push_back(vm->fromAeValue(argv[i])); }
        catch (const exception& e) {
            vm->nativeCallError = string("func_call 的参数非法: ") + e.what();
            if (err) *err = vm->nativeCallError.c_str();
            return 0;
        }
    }
    Value res;
    if (!vm->callValueFromNative(Value::makeFunc(vm->funcHandles[(size_t)h - 1]), args, &res, &vm->nativeCallError)) {
        if (err) *err = vm->nativeCallError.c_str();
        return 0;
    }
    if (out) *out = vm->toAeValue(res);
    return 1;
}

static int api_table_get(AeVM* v, AeValue t, AeValue k, AeValue* out) {
    if (out) { out->kind = AE_NULL; out->i = 0; out->f = 0; out->p = nullptr; }
    const Table* tp = (const Table*)t.p;
    if (!tp || !out) return 0;
    try {
        VM* vm = asVM(v);
        Value key = vm->fromAeValue(k);      // 键只可能是 int/bool/float/string
        Value val;
        if (!tp->get(key, val)) return 0;
        // 值可以是任意类型：字符串/表都是【借用】表里那份，父表活着就有效，
        // 库可以拿嵌套的表继续调 table_get / table_len。
        *out = vm->toAeValue(val);
        return 1;
    } catch (...) {
        return 0;
    }
}
static int api_table_len(AeVM* v, AeValue t) {
    (void)v;
    const Table* tp = (const Table*)t.p;
    return tp ? (int)tp->length() : 0;
}

static const AeApi* aeNativeApi() {
    static const AeApi kApi = {
        AE_ABI_VERSION,
        api_push_null, api_push_int, api_push_float, api_push_bool, api_push_str, api_push_table,
        api_make_str, api_new_table, api_table_set,
        api_set_error, api_raise,
        api_arg_count, api_arg_get, api_script_path,
        api_table_get, api_table_len,
        api_func_retain, api_func_release, api_func_call,
        { nullptr }
    };
    return &kApi;
}

// =============================================================================
//  main
// =============================================================================
int main(int argc, char** argv) {
    aecon::install();    // ★ 第一件事：把 cout/cerr/cin 接到控制台（GBK 的 cmd 里中文不乱码）
    string path;
    size_t maxDepth = 10000;
    vector<string> scriptArgs;

    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        // ★ 脚本路径【之后】的参数一律原样传给脚本，不再当作 ae 自己的选项：
        //   否则脚本永远拿不到 -h / --help / --version（会被 VM 自己吃掉）。
        if (!path.empty()) { scriptArgs.push_back(a); continue; }
        if ((a == "--max-depth" || a == "-m") && i + 1 < argc) {
            try { maxDepth = (size_t)stoull(argv[++i]); }
            catch (...) { aediag::printBrief(AE_E_USAGE, "--max-depth 需要一个正整数"); return 3; }
            continue;
        }
        if (a == "-h" || a == "--help") {
            cout << "Ae 虚拟机 (ae) 1.1\n"
                    "用法: ae [--max-depth N] <file.aeo> [脚本参数...]\n"
                    "  --max-depth N, -m N   调用深度上限（默认 10000）\n"
                    "  -h, --help            显示帮助\n"
                    "  --version             显示版本\n"
                    "<file.aeo> 之后的参数原样传给脚本（os.args() / args 库可见）\n"
                    "退出码: 0 成功  2 运行错误  3 用法/IO 错误\n";
            return 0;
        }
        if (a == "--version") { cout << "ae 1.1 (AeBc 1.7)\n"; return 0; }
        path = a;
    }

    if (path.empty()) {
        cout << "Ae 虚拟机 (ae) - 输入字节码文件路径: ";
        getline(cin, path);
        if (path.empty()) { aediag::printBrief(AE_E_USAGE, "未提供输入文件"); return 3; }
    }
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
        path = path.substr(1, path.size() - 2);

    VM vm;
    vm.maxCallDepth = maxDepth;
    vm.scriptArgs   = scriptArgs;
    try {
        vm.load(path);
    } catch (const LoadIOError& e) {
        // 还没开始执行：一行式报错 + 退出码 3（用法/IO 错误）
        aediag::printBrief(AE_E_FILE, e.what());
        return 3;
    } catch (const exception& e) {
        renderRunError(vm, e.what(), vm.thrownCode.empty() ? nullptr : vm.thrownCode.c_str());
        return 2;
    }
    try {
        vm.run();
    } catch (const ExitException& ex) {
        return ex.code;                      // ★ P1-6：exit(n) 直接带退出码结束
    } catch (const exception& e) {
        renderRunError(vm, e.what(), vm.thrownCode.empty() ? nullptr : vm.thrownCode.c_str());
        return 2;                            // ★ 运行错误退出码 = 2
    }
    return 0;
}