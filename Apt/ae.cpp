// Ae 虚拟机 (ae) —— 执行 aec 编译出的字节码
// 当前能力
// · 函数：自定义函数、块作用域、递归、闭包与开放上值、多返回值
// · 值：null / int / float / string / bool / 通用表 / 函数值，type() 取类型名
// · 通用表：数组段（1-based 连续整数键）+ 哈希段（其余键），引用语义
// · 控制流：if / while / do-while / for 数值与 for-in / break N / continue N
// · 错误处理：try { } catch (e)，e 带 code / kind / msg / line / col / file / stack
// · 原生库：imp <库名> 加载 .dll / .so，经 AeApi ABI 双向调用
// · 其他：位运算与幂、format() printf 风格、深拷贝 / 深相等、弱值表
//
// 值的基石（Value Foundation）设计约定：
// ① 类型标签 ValueKind：VAL_NULL 表示"无值"，仅与自身相等。
// ② 判等（== / !=）采用严格类型 + 值语义：
// 与 null 比较永远合法（只有 null == null 为 true）
// int 与 float 之间运行时报错：两者是同一数值域的不同表示，
// 静默判为不等最易踩坑（写 1.0 或 float(x) 即可）
// 其余类型不同 → false / true（不同类型本来就不相等）
// ③ 表键（TableKey）只允许 int / string / bool；float / null / 表作键一律运行时
// 报错。哈希由 TableKey::hash() 给出，与上面的严格判等保持一致。
// ④ type(v) 内置函数 → 返回类型名字符串，与 ValueKind 一一对应。
// ⑤ 表（TABLE）的 == / != 按引用身份比较（同一个表对象才相等）。
//
// 历史修复（P0 批次：语义错误与未定义行为）：
// · P0-2 常量池支持 INT64（tag=5）：运行期是 int64，字面量此前却按 int32 存，
// `prln(3000000000)` 会抛 C++ 原始异常 "stoi argument out of range"。
// · P0-3 判等与大小比较分开定案：
// / != —— 与 null 比较永远合法；int 与 float 之间报类型错；
// 其余类型不同 → false / true
// < <= > >= —— 类型不同一律报类型错；null / 表没有大小关系
// （旧实现在跨类型时静默返回 false：`f = 1.5` 时 `f > 1` 得 false，
// 而同一份代码里 `f + 1` 却能自动提升成 2.5，静默给错答案。）
// · P0-4 bool 不再是数值：`true + 1` 报类型错（它有自己的 type()，
// 且 `true == 1` 为 false，允许相加会自相矛盾）。显式 int(true) 仍然可用。
// · P0-6 整数溢出定义为二进制回绕（用无符号中转，消除有符号溢出 UB，
// 并显式定义 INT64_MIN / -1 这两个 C++ 未定义运算的结果）。
// · P0-7 int()/float() 转换失败抛运行时错误，不再静默返回 0。
//
// 内存管理：
// · 字符串继续使用 shared_ptr；表与闭包由 shared_ptr 保证即时释放。
// · VM 额外维护分代 + 增量的非移动式环收集器（详见下方 GC 段落）：
// 只处理引用计数收不掉的「环」——标记出不可达对象后清空其出边，让计数归零；
// 标记切成小步在字节码安全点推进，写屏障让期间被改写的对象退回灰色重扫。
// · GC 不移动对象，开放上值与原生 ABI 的借用指针在有效期内保持稳定。

#ifdef _WIN32
  #define _CRT_SECURE_NO_WARNINGS
#endif

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <memory>
#include <type_traits>
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
#include <unordered_set>
#include <cstdio>
#include <cstdlib>
#include <new>               // placement new（Value 的 union 成员激活要用）
#include "ae_libhost.h"      // 原生库加载 + ABI（可选：动态库扩展）
#include "ae_builtin.h"      // 内置库（io / os / path 静态链入，无需 .so/.dll）
#include "ae_diag.h"         // 诊断系统
#include "ae_console.h"     // 控制台中文：绕开代码页
#include "ae_path.h"        // 路径编码：UTF-8 / ANSI / 宽字符

// 原生库 API 表（定义在文件后部，这里先声明）
class VM;
static const AeApi* aeNativeApi();
// 运行期错误分类（定义在文件后部；VM 构造错误表要用）
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
    enum { UTF8 = 1, INT32 = 2, DOUBLE = 3, NULLV = 4, INT64 = 5 } tag;   // 与编译器一致
    string   s;
    int32_t  i = 0;
    int64_t  i64 = 0;    // P0-2
    double   d = 0;
};

// 运行时值类型标签
// 通用表（table）：数组 / 字典合二为一
// 连续整数键（1-based）存在数组段 `arr`，实现 O(1) 随机访问与 #t
// 其余键（int / string / bool）存在哈希段 `mapv`（开放寻址，不再是 std::map）
// 稀疏整数键（下标 > 数组长度 + 8）也走哈希段，避免开出百万个空槽
// 赋值时按键自动分流；读取未定义键 → 返回 null（与 Lua 一致）
// Value / TableKey 均为 24 字节：标量或引用共享同一段 union 存储（彼此互斥）
enum ValueKind {
    VAL_NULL  = 0,
    VAL_INT   = 1,
    VAL_DOUBLE = 2,
    VAL_STR   = 3,
    VAL_BOOL  = 4,
    VAL_TABLE = 5,  // 原 VAL_ARR 位置升级为"通用表"
    VAL_FUNC  = 6,  // 闭包（函数值）
    VAL_CO    = 7   // 协程（独立的执行状态）
};

struct Value;    // 前置声明
struct Closure;  // 前置声明（闭包，见 Value 之后）
struct Coroutine;// 前置声明（协程，见 Value 之后）
struct Table;
struct Upvalue;

// 分代 + 增量 GC 的对象头（每个 Table / Closure 各一份）
// · color：三色标记。白=未访问（可能死）/ 灰=待扫描 / 黑=已扫描（本轮存活）
// · age ：经历过的收集次数。达到 GC_PROMOTE_AGE 即晋升老年代；
// minor 只扫描并回收年轻对象，"老 → 年轻" 的边由 remembered set 覆盖
// · weakValues：弱值表。值不参与标记，标记结束后清除指向已死对象的条目
// GC 不移动对象，开放上值与原生 ABI 的借用指针在有效期内保持稳定。
enum : uint8_t { GC_WHITE = 0, GC_GRAY = 1, GC_BLACK = 2 };
enum : uint8_t { GC_PROMOTE_AGE = 2 };

struct GcHeader {
    uint8_t color      = GC_WHITE;
    uint8_t age        = 0;
    bool    weakValues = false;   // 弱值表
    bool    remembered = false;   // 是否已在 remembered set 中（去重用）
    bool    immortal   = false;   // 永久对象（常量池字符串）：永不回收、也不必重标
};

// 写屏障：实现放在 VM 之后（此处只有前置声明），运行时经 g_gcVM 找到当前 VM。
// 单线程 VM，因此用全局指针而非 thread_local。
extern VM* g_gcVM;
void gcBarrierTable(Table* t);                        // 表内容被改写
void gcBarrierTableValue(Table* t, const Value& v);   // 表内容被改写且已知新值
void gcBarrierClosure(Closure* c);                    // 闭包上值被改写
void gcBarrierValue(const Value& v);                  // 根区域（栈/局部/全局）写入
void gcBarrierUpvalue(Upvalue* up);         // 上值被改写/关闭

// 哈希段：键（严格判等 + 可哈希） → 值
// GC 托管的字符串
// 继承 std::string 是为了让所有 `str->c_str() / *str / str->size()` 这类调用
// 一字不改继续工作 —— 否则这次瘦身要改上百处。GcHeader 放后面不影响 upcast。
// 生命周期完全交给 GC：sweep 里 delete，不再有引用计数。
struct GcString : std::string {
    GcHeader gc;
    // 哈希缓存：字符串不可变，哈希值算一次就永久有效。
    // 表查找是 Ae 最热的路径，原来每查一次键都要把整串重新哈希一遍
    // （长键尤其吃亏）。kNoHash 表示"还没算过"。
    static const size_t kNoHash = ~(size_t)0;
    mutable size_t hashCache = kNoHash;

    size_t hashValue() const {
        size_t h = hashCache;
        if (h == kNoHash) {
            h = std::hash<std::string>()(*static_cast<const std::string*>(this));
            hashCache = h;
        }
        return h;
    }

    GcString() = default;
    GcString(const std::string& x) : std::string(x) {}
    GcString(std::string&& x) : std::string(std::move(x)) {}   // 搬家，不拷贝
    GcString(const char* x, size_t n) : std::string(x, n) {}
};
// 实现在 VM 之后（需要 g_gcVM），这里只声明
GcString* aeAllocString(const std::string& s);
GcString* aeAllocString(std::string&& s);

struct TableKey {
    using StrPtr = GcString*;

    ValueKind kind;
    // 同理瘦身（40 → 24 字节）：i / f / str 互斥，不必各自占位。
    // 它是 std::map 的键，节点数量多，省下来的都是实打实的内存与缓存。
    union {
        int64_t i;
        double  f;
        StrPtr  str;
    };

    TableKey() : kind(VAL_NULL), i(0) {}
    void release()                  { kind = VAL_NULL; }
    void copyFrom(const TableKey& o){ kind = o.kind; i = o.i; }
    void moveFrom(TableKey& o)      { kind = o.kind; i = o.i; o.kind = VAL_NULL; }
    // 字符串键：union 成员不能直接赋值，必须 placement new
    void setStr(const StrPtr& s) { kind = VAL_STR; str = s; }

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

// （Table 的定义已下移到 Value / HashMap 之后 —— 哈希槽里要放完整的 Value）

// Value：Ae 里所有值的载体
// 布局瘦身（64 → 24 字节）
// str / tab / fn 三者天然互斥 —— 一个值只可能是其中一种 —— 原来却各自占满
// 16 字节，48 字节白烧在每一个值上：栈槽、局部槽、表数组段、上值、常量池。
// 并进同一个 union 之后，整个值只剩 kind(4) + 对齐填充(4) + union(16) = 24 字节。
// 代价：union 里含 shared_ptr，构造 / 析构 / 拷贝 / 移动必须手写；
// 对标量成员（i / f）仍然只是直接赋值的平凡操作，没有额外开销。
struct Value {
    using StrPtr = GcString*;
    using TabPtr = Table*;
    using FunPtr = Closure*;
    using CoPtr  = Coroutine*;

    ValueKind kind;
    union {
        int64_t  i;
        double   f;
        StrPtr  str;    // 仅 VAL_STR
        TabPtr  tab;    // 仅 VAL_TABLE
        FunPtr  fn;     // 仅 VAL_FUNC
        CoPtr   co;     // 仅 VAL_CO
    };

    // 生命周期（union 含非平凡成员，必须手写这五个）
    Value() : kind(VAL_NULL), i(0) {}
    // 拷贝 / 移动 / 析构全部交给编译器生成：union 里全是 8 字节平凡位模式，
    // 隐式拷贝按对象表示整体搬运，与手写版等价，且让 Value 成为 trivially copyable。

    // 标量：平凡成员，直接初始化
    Value(int64_t v) : kind(VAL_INT),    i(v) {}
    Value(double v)  : kind(VAL_DOUBLE), f(v) {}
    Value(bool b)    : kind(VAL_BOOL),   i(b ? 1 : 0) {}
    // 引用类型：union 成员必须用 placement new 激活
    explicit Value(StrPtr s) : kind(VAL_STR),   str(s) {}
    explicit Value(TabPtr t) : kind(VAL_TABLE), tab(t) {}
    explicit Value(FunPtr c) : kind(VAL_FUNC),  fn(c)  {}
    explicit Value(CoPtr  o) : kind(VAL_CO),     co(o)  {}
    Value(const string& s) : kind(VAL_STR), str(aeAllocString(s)) {}
    // 右值：直接把内容搬进 GcString，省掉一次整串拷贝（拼接热路径很吃这个）
    Value(string&& s)      : kind(VAL_STR), str(aeAllocString(std::move(s))) {}

    // union 里全是 8 字节平凡位模式（int64 / double / 指针），没有需要析构的成员：
    // 拷贝 = 按 int64 整体搬运（double 与指针的位模式原样保留），析构 = 什么都不做。
    // 这就是去掉 shared_ptr 之后最大的收益：拷贝一个值不再有原子引用计数。
    void release()                 { kind = VAL_NULL; }
    void copyFrom(const Value& o)  { kind = o.kind; i = o.i; }
    void moveFrom(Value& o)        { kind = o.kind; i = o.i; o.kind = VAL_NULL; }

    static Value makeNull()  { return Value(); }
    static Value makeTable(TabPtr t)  { return Value(t); }
    static Value makeFunc (FunPtr c)  { return Value(c); }
    static Value makeCo    (CoPtr  o)  { return Value(o); }

    bool isTable() const { return kind == VAL_TABLE; }
    bool isFunc()  const { return kind == VAL_FUNC; }
    bool isCo()    const { return kind == VAL_CO; }
    bool isNull()  const { return kind == VAL_NULL; }
};

// 布局回归保护：一旦有人往 Value / TableKey 里加成员而不放进 union，
// 尺寸就会悄悄长回去，这里直接编译失败，比性能退化后才发现好得多。
static_assert(sizeof(Value)    == 16, "Value 变大了：str/tab/fn 必须是裸指针且留在同一个 union 里");
static_assert(sizeof(TableKey) == 16, "TableKey 变大了：i/f/str 必须留在同一个 union 里");
static_assert(std::is_trivially_copyable<Value>::value, "Value 应当是平凡可拷贝的（不再含 shared_ptr）");

// 闭包：上值 + 函数值
// Upvalue = 被闭包捕获的"外部变量"。open 非空时它直接指向某个调用帧的
// locals[slot] —— 所以闭包和原作用域共享同一个变量(引用捕获)，两边改都生效。
// 帧退出时 close() 把值搬进 closed，之后闭包独立持有它（这就是闭包能"活过"
// 创建它的函数的原因，也是必须 close 的原因：不 close 就会指向已释放的帧）。
struct Upvalue {
    GcHeader gc;            // GC 头：Upvalue 现在也归 GC 统一回收
    Value* open = nullptr;       // 指向某帧 locals[slot]；nullptr = 已关闭
    Value  closed;               // 关闭后的副本

    void close() { if (open) { closed = *open; open = nullptr; } }
    Value& get() { return open ? *open : closed; }
};

// AOT 生成的函数指针：返回"返回值个数"，值写进 rv[]；up 是闭包的上值数组（可为 nullptr）
typedef int (*AeAotFn)(class VM*, int, Value*, Value*, Upvalue**);

// 闭包 = 函数原型（funcId）+ 捕获到的上值表
struct Closure {
    GcHeader gc;                                   // GC 头（颜色 / 代龄）
    uint16_t funcId = 0;
    std::vector<Upvalue*> ups;
    AeAotFn  aotFn = nullptr;                      // AOT：非空则走原生调用，不用 funcId
};

// 值的基石：严格判等 / 哈希 / 类型名

// TableKey：哈希段的键
// 允许类型：int / string / bool（float 也允许，但建议脚本层避免）
// 严格判等（类型不同即不等），与 valueEquals 一致
inline bool TableKey::equals(const TableKey& o) const {
    if (kind != o.kind) return false;
    switch (kind) {
        case VAL_INT:   return i == o.i;
        case VAL_BOOL:  return i == o.i;
        case VAL_DOUBLE: return f == o.f;
        case VAL_STR: {
            if (str == o.str) return true;      // 同上：同一对象 → 必然相等
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
            if (!str) return h;
            // 用字符串自带的哈希缓存：同一个键被反复查找时只算一次
            return h ^ str->hashValue();
        }
        default:        return h;
    }
}

// Value → TableKey（仅限允许的键类型）
inline TableKey toKey(const Value& v) {
    TableKey k;
    switch (v.kind) {
        case VAL_INT:   k.kind = VAL_INT;  k.i = v.i; break;
        case VAL_BOOL:  k.kind = VAL_BOOL; k.i = v.i; break;
        case VAL_STR:   k.setStr(v.str); break;      // union 成员要 placement new
        case VAL_DOUBLE:                       // P2-17：浮点键定案为错误
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
            // 指针快路径：字面量在 constString 里已复用（同一下标 → 同一对象），
            // 同一 GcString 内容必然相同（不可变），可以先 O(1) 判掉最常见的情况。
            if (a.str == b.str) return true;
            const char* sa = a.str ? a.str->c_str() : "";
            const char* sb = b.str ? b.str->c_str() : "";
            return strcmp(sa, sb) == 0;
        }
        case VAL_TABLE: return a.tab == b.tab;   // 表按引用身份比较
        case VAL_FUNC:  return a.fn == b.fn;     // 函数按闭包身份比较
        case VAL_CO:    return a.co == b.co;     // 协程按身份比较
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
        case VAL_CO:     return "coroutine";
        default:         return "unknown";
    }
}

// HashMap：表的哈希段（开放寻址 + 线性探测）
// 原来是 std::map（红黑树）：每次插入都要 new 一个节点，查找 O(log n)，
// 而且一路指针追逐把缓存打穿 —— 表越大越明显。
// 换成扁平的开放寻址数组之后：
// · 查找 / 插入摊还 O(1)，槽在内存里连续，一个缓存行能挨着看好几个槽
// · 整段就是一块 vector，彻底没有逐节点分配
// · 删除留"墓碑"而不是搬移，免得把别人的探测链截断
// 注意：键只允许 int / bool / string（toKey 已经把 float / 表 / null 挡在外面），
// 所以 kind == VAL_NULL 可以安全地表示"这槽空着"，不必再加一层标志数组。
// 注意：迭代顺序：槽序 ≠ 键序。语言规范承诺哈希段按键排序输出（打印表、
// 取键列表），那两处请用 forEachSorted；GC 扫描这类不在乎顺序的用槽序。
struct HashMap {
    static constexpr uint8_t EMPTY = 0;   // 从没用过：探测到这里就可以收工
    static constexpr uint8_t FULL  = 1;   // 在用
    static constexpr uint8_t TOMB  = 2;   // 删过：探测还得继续往前走

    struct Slot {
        TableKey key;
        Value    val;
        uint8_t  state = EMPTY;
    };

    std::vector<Slot> slots;
    size_t count = 0;      // 在用元素数（等价于原来的 mapv.size()）
    size_t used  = 0;      // 在用 + 墓碑，决定什么时候该重建

    bool   empty() const { return count == 0; }
    size_t size()  const { return count; }
    void   clear()       { slots.clear(); count = 0; used = 0; }

    // 查找
    const Slot* find(const TableKey& k) const {
        if (slots.empty()) return nullptr;
        const size_t mask = slots.size() - 1;        // 容量恒为 2 的幂
        const size_t start = k.hash() & mask;
        for (size_t probe = 0; probe <= mask; probe++) {
            const Slot& s = slots[(start + probe) & mask];
            if (s.state == EMPTY) return nullptr;    // 撞见真空 → 后面不可能还有
            if (s.state == FULL && s.key.equals(k)) return &s;
        }
        return nullptr;
    }
    Slot* find(const TableKey& k) {
        return const_cast<Slot*>(static_cast<const HashMap*>(this)->find(k));
    }

    bool get(const TableKey& k, Value& out) const {
        if (const Slot* s = find(k)) { out = s->val; return true; }
        return false;
    }

    // 写入
    void set(const TableKey& k, const Value& v) {
        if (Slot* hit = find(k)) { hit->val = v; return; }   // 已存在：只改值
        insertNew(k, v);
    }

    bool erase(const TableKey& k) {
        Slot* s = find(k);
        if (!s) return false;
        eraseSlot(s);
        return true;
    }
    void eraseSlot(Slot* s) {
        s->key = TableKey();     // 放掉键（字符串引用）
        s->val = Value();        // 放掉值
        s->state = TOMB;
        count--;
        // used 不减：墓碑得留在探测链里，否则会截断别人查找时的退路
    }

    // 迭代（槽序，用于 GC 扫描这类不在乎顺序的场合）
    struct Iter {
        Slot* p; Slot* e;
        void skip() { while (p != e && p->state != FULL) ++p; }
        Iter& operator++() { ++p; skip(); return *this; }
        bool operator!=(const Iter& o) const { return p != o.p; }
        Slot& operator*()  const { return *p; }
        Slot* operator->() const { return p; }
    };
    struct ConstIter {
        const Slot* p; const Slot* e;
        void skip() { while (p != e && p->state != FULL) ++p; }
        ConstIter& operator++() { ++p; skip(); return *this; }
        bool operator!=(const ConstIter& o) const { return p != o.p; }
        const Slot& operator*()  const { return *p; }
        const Slot* operator->() const { return p; }
    };
    Iter begin()      { Slot* b = slots.data(); Iter it{b, b + slots.size()}; it.skip(); return it; }
    Iter end()        { Slot* b = slots.data(); return Iter{b + slots.size(), b + slots.size()}; }
    ConstIter begin() const { const Slot* b = slots.data(); ConstIter it{b, b + slots.size()}; it.skip(); return it; }
    ConstIter end()   const { const Slot* b = slots.data(); return ConstIter{b + slots.size(), b + slots.size()}; }

    // 需要"顺序确定"的场合（打印表 / 取键列表）走这里：
    // 哈希表的槽序是随插入历史变的，而规范承诺的是键序，所以这里排一次。
    template <class F>
    void forEachSorted(F&& f) const {
        std::vector<const Slot*> v;
        v.reserve(count);
        for (const Slot& s : slots) if (s.state == FULL) v.push_back(&s);
        std::sort(v.begin(), v.end(),
                  [](const Slot* a, const Slot* b) { return a->key < b->key; });
        for (const Slot* s : v) f(*s);
    }

private:
    // 负载到 0.7 就重建；如果只是墓碑堆得多而元素不多，就地同容量重建清掉墓碑
    void maybeGrow() {
        if (slots.empty()) { rehash(8); return; }
        if ((used + 1) * 10 >= slots.size() * 7) {
            size_t want = slots.size();
            if (count * 10 >= want * 5) want *= 2;   // 元素真的多 → 翻倍
            rehash(want);
        }
    }

    void insertNew(const TableKey& k, const Value& v) {
        maybeGrow();
        const size_t mask = slots.size() - 1;
        const size_t start = k.hash() & mask;
        Slot* tomb = nullptr;
        for (size_t probe = 0; probe <= mask; probe++) {
            Slot& s = slots[(start + probe) & mask];
            if (s.state == FULL) continue;                 // 已确认不存在，跳过
            if (s.state == TOMB) { if (!tomb) tomb = &s; continue; }
            placeInto(tomb ? *tomb : s, tomb == nullptr, k, v);
            return;
        }
        // 整段都是墓碑、没撞见真空：仍能复用墓碑（maybeGrow 一般会先把这情况清掉）
        if (tomb) { placeInto(*tomb, false, k, v); return; }
        throw std::runtime_error("哈希表内部错误：找不到可插入的槽");
    }

    void placeInto(Slot& dst, bool isFreshEmpty, const TableKey& k, const Value& v) {
        if (isFreshEmpty) used++;   // 只有新占一个空位才增加占用；复用墓碑则不变
        dst.key   = k;
        dst.val   = v;
        dst.state = FULL;
        count++;
    }

    void rehash(size_t newCap) {
        std::vector<Slot> old = std::move(slots);
        slots.clear();
        slots.resize(newCap);          // 新槽 state 一律是 EMPTY
        count = 0;
        used  = 0;
        const size_t mask = slots.size() - 1;
        for (Slot& s : old) {
            if (s.state != FULL) continue;
            const size_t start = s.key.hash() & mask;
            for (size_t probe = 0; probe <= mask; probe++) {
                Slot& d = slots[(start + probe) & mask];
                if (d.state == EMPTY) {
                    d.key   = std::move(s.key);
                    d.val   = std::move(s.val);
                    d.state = FULL;
                    count++; used++;
                    break;
                }
            }
        }
    }
};

struct Table {
    GcHeader gc;                                   // GC 头（颜色 / 代龄 / 弱值标记）

    // 数组段：逻辑索引 1 → 内部下标 0，尾部自动扩容（Lua 风格）
    vector<Value> arr;

    // 哈希段：开放寻址（原 std::map 红黑树）
    HashMap mapv;

    bool get(const Value& key, Value& out) const;
    void set(const Value& key, const Value& val);
    size_t length() const;      // #t：连续整数键前缀长度
};

// Table 成员函数实现（此时 Value / HashMap 都已完整定义）
inline bool Table::get(const Value& key, Value& out) const {
    // 整数键：优先走数组段（支持连续 1-based 索引）
    if (key.kind == VAL_INT) {
        int64_t idx = key.i;
        if (idx >= 1 && idx <= (int64_t)arr.size()) {
            out = arr[(size_t)idx - 1];
            return true;
        }
    }
    return mapv.get(toKey(key), out);   // 未找到 → 不动 out（由调用方填 null）
}

inline void Table::set(const Value& key, const Value& val) {
    gcBarrierTableValue(this, val);   // 写屏障：增量期把黑对象退回灰 / 分代期登记老→年轻
    if (key.kind == VAL_INT) {
        // 整数键 → 数组段（Lua 风格：允许尾部追加，中间空洞补 null）
        int64_t idx = key.i;
        if (idx < 1) throw runtime_error("表索引必须为正整数（1-based），得到 " + to_string(idx));
        // P2-15：稀疏整数键（远超当前数组长度）改走哈希段，
        // 避免 t[1000000] = "x" 真的开出百万个槽
        if (idx <= (int64_t)arr.size() + 8) {
            size_t i = (size_t)idx - 1;
            if (i >= arr.size()) arr.resize(i + 1, Value());   // 空洞补 null
            arr[i] = val;
        } else {
            mapv.set(toKey(key), val);
        }
        return;
    }
    // 其余键 → 哈希段
    mapv.set(toKey(key), val);
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

// 函数定义 / 调用帧
// 上值描述：本闭包的某个上值从哪来
// fromParentLocal = true → 抓父帧的 locals[index]（引用捕获，共享变量）
// fromParentLocal = false → 继承父闭包的第 index 个上值（多层嵌套的中转）
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
    std::vector<UpvalDesc> ups;   // 闭包：上值描述表
    // 默认参数（minor>=8）：按参数位排，0xFFFF = 该位没有默认值（必传）
    // 只有字面量能当默认值，所以这里存的是常量池下标
    std::vector<uint16_t> defaultConsts;
    // 变参（minor>=8）：变参名的槽位（0xFFFF = 无变参）
    uint16_t variadicSlot = 0xFFFF;
    bool isVariadic() const { return variadicSlot != 0xFFFF; }
    // 固定参数个数（不含变参那位）
    uint16_t fixedParamCount() const {
        return isVariadic() ? (paramCount > 0 ? paramCount - 1 : 0) : paramCount;
    }
    uint16_t minParamCount() const {
        uint16_t lim = fixedParamCount();
        uint16_t n = 0;
        for (uint16_t i = 0; i < defaultConsts.size() && i < lim; i++) {
            if (defaultConsts[i] != 0xFFFF) break;
            n++;
        }
        return n;
    }
};

// defer：一条已登记、尚未执行的延迟调用
struct DeferRec {
    uint16_t scopeId = 0;          // 所属块作用域；退出该块时执行
    bool     native  = false;      // true = 原生库函数（按名查 nativeFns）
    uint16_t constIdx = 0;         // native 时：常量池里 "mod.func" 的下标
    uint8_t  builtinId = 0xFF;     // 内置函数（prln / len ...）时：内置 id
    uint8_t  argc = 0;
    Value    callee;               // 非 native：被调用的函数值
    vector<Value> args;            // defer 语句执行时就已求值（Go 语义）
};

struct CallFrame {
    // defer 表：登记顺序 = 登记先后；执行时按反序
    vector<DeferRec> defers;
    uint16_t funcId;
    size_t   returnPC;
    size_t   stackBase;          // P1-10：进入本函数时的操作数栈高度
    uint8_t  want;               // P1-5：调用方希望得到几个返回值
    vector<Value> locals;
    // 闭包支持
    Closure* closure;                       // 本帧执行的闭包（可为空 = 普通函数）
    vector<pair<uint16_t, Upvalue*>> openUps;  // 本帧里打开的上值：slot → Upvalue

    // 帧退场前必须把所有开放上值"关掉"（否则闭包会指向已释放的 locals）
    void closeUpvalues() {
        for (auto& kv : openUps) {
            kv.second->close();
            gcBarrierUpvalue(kv.second);   // 上值关闭后值搬进堆：让进行中的标记看到它
        }
        openUps.clear();
    }
};

// 错误处理器的栈条目（从 VM 里提出来：协程也要保存自己的一份）
struct Handler { size_t frameDepth, stackSize, landing; };

// 协程（stackful / 非对称，Lua 风格）
// · 每个协程有独立的操作数栈与调用栈，切换时 move 进出 VM 成员
// · resume 进、yield 出，控制权始终回到调用者（非对称）
// · yield 可以发生在任意深度的普通函数里 —— wait() 不必标 async
// 挂起期间协程的 stack / callStack 必须被 GC 当作根扫描，
// 否则里面引用的表会被回收成悬垂指针（本实现最容易漏、也最难查的一条）。
struct Coroutine {
    GcHeader gc;
    enum Status { SUSPENDED, RUNNING, DEAD };
    Status  status  = SUSPENDED;
    bool    started = false;         // 还没跑过第一帧
    Value   fn;                      // 协程函数（启动前持有）
    Coroutine* parent = nullptr;     // 谁 resume 了我 —— yield 时回到它
    bool    isMain   = false;        // 主执行流（VM 自己那一份）

    // 挂起态的完整执行状态
    vector<Value>     stack;
    vector<CallFrame> callStack;
    vector<Handler>   handlers;
    size_t pc = 0, instrPC = 0;
    uint8_t yieldWant = 0;          // yield 处调用方想要几个返回值
    bool   hasState = false;         // 是否持有有效的挂起状态
    string deadReason;
};

// 协程 yield 用异常跨越 invoke → 指令分派 → execLoop 的 C++ 栈。
// yield 是内置函数（同步 C++ 调用），没法"自动中断"主循环，
// 所以抛一个信号，让 execLoop 直接返回，把执行状态留在原地（= 挂起点）。
// 故意不继承 std::exception，免得被通用的异常处理吞掉。
struct YieldSignal {};

// 1.7：读一个 FUNC_DEF 头。次版本 ≥7 时头里多一张上值描述表
// （闭包要按这张表在创建时刻抓父帧的局部变量）
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
    fd.defaultConsts.clear();
    if (minor >= 8) {
        uint16_t n = readU16(c, p);
        for (uint16_t i = 0; i < n; i++) fd.defaultConsts.push_back(readU16(c, p));
        fd.variadicSlot = readU16(c, p);
    }
    fd.entryPC = (uint32_t)p;
    return p;
}

// P1-6：exit(n) 用这个异常直接结束进程并带上退出码
struct ExitException { int code; };

// 打不开 .aeo 不是「运行错误」，是用法/IO 错误：
// 单独一个类型，好在 main 里分成 E0002 + 退出码 3（和 ae -h 里写的退出码表一致）
struct LoadIOError : std::runtime_error {
    explicit LoadIOError(const std::string& m) : std::runtime_error(m) {}
};

// P1-12 / P2-11：UTF-8 工具
// 把字符串按码点切块，返回每块的 (偏移, 字节数)
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
// P1-12：UTF-8 字符数
static int64_t utf8Length(const std::string& s) {
    return (int64_t)utf8Chars(s).size();
}
// 切片用：按 1-based 整数下标取数组段的值（越界给 null，不抛）
static Value valueAt(const Table& t, int64_t i) {
    if (i >= 1 && i <= (int64_t)t.arr.size()) return t.arr[(size_t)i - 1];
    Value out;
    if (t.get(Value(i), out)) return out;
    return Value();
}
// 切片 / 负索引用：表的"长度"就是数组段长度（与 len() 一致）
static size_t lenOfTable(const Table& t) { return t.length(); }
// P2-11：第 i 个字符（1-based，按码点），越界返回空串
static std::string utf8CharAt(const std::string& s, int64_t i) {
    if (i < 1) return std::string();
    auto cs = utf8Chars(s);
    if ((size_t)i > cs.size()) return std::string();
    return s.substr(cs[(size_t)i - 1].first, cs[(size_t)i - 1].second);
}
// P2-9：浮点最短往返表示。此前 prln(x) 走默认流格式、str(x) 走 fixed 6 位小数，
// 同一个值会给出两种字符串（1e20 → "1e+20" vs "100000000000000000000.0"）。
// 现在两者共用这一条规则，并保证 float(str(x)) == x。
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

// 虚拟机
class VM {
public:
    vector<Const> constPool;
    // 常量池字符串的运行时缓存：同一个字面量只分配一次 GcString。
    // 字面量不可变、又随程序同生共死，所以可以直接当永久根复用，
    // 不必每次 LOAD_CONST 都新建（字符串密集型代码能省下大半分配）。
    vector<GcString*> constStrCache;
    GcString* constString(size_t idx) {
        if (idx >= constStrCache.size()) constStrCache.resize(idx + 1, nullptr);
        GcString*& p = constStrCache[idx];
        if (!p) {
            p = allocString(constPool[idx].s);
            p->gc.color    = GC_BLACK;
            p->gc.immortal = true;       // 永久根：不当垃圾，也不必每轮重标
        }
        return p;
    }
    vector<uint8_t> code;
    size_t pc = 0;

    vector<Value> stack;
    // 协程：gcCoroutines 是全部协程（GC 拥有）；curCo 是当前执行流
    // （主执行流也用一个 Coroutine 对象表示，这样切换逻辑对主/子一视同仁）。
    vector<Coroutine*> gcCoroutines;
    Coroutine* curCo = nullptr;
    vector<Value> yieldValues;            // yield 传出的值（跨越 execLoop 边界）
    bool yieldPending = false;            // execLoop 因 yield 而返回（不能直接抛穿）
    vector<Value> globals;
    vector<FuncDef> funcs;

    vector<CallFrame> callStack;
    vector<Value>* currentLocals = nullptr;

    // P1-2：字节码自带的调试信息与校验用数据
    string srcName = "<source>";
    string scriptDir;                                  // .aeo 所在目录（搜库用）
    string srcPath;                                    // .aeo 的完整路径（脚本可查）
    vector<string> scriptArgs;                         // 传给脚本的命令行参数
    vector<pair<uint16_t,std::string>> funcNames;
    struct LineEntry { uint32_t off, line, col; uint16_t file; };
    vector<string> fileNames;                       // 源文件表（模块编译后不止一个）
    map<int,string> srcCache;                       // 每个源文件的文本（诊断摘录用）
    vector<LineEntry> lineTable;                    // 按 offset 升序
    size_t instrPC = 0;                             // 当前正在执行的指令起点
    // 错误捕获：处理器栈（帧深度 / 操作数栈高度 / catch 落地点）
    vector<Handler> handlers;
    string thrownCode;                              // 显式抛出时指定的错误码
    string srcText;                                 // 源码（出错时摘录那一行）
    bool   srcLoaded = false;
    // P1-13：调用深度上限（可用 --max-depth 调整）
    size_t maxCallDepth = 10000;
    int    exitCode = 0;

    // 原生库状态
    map<string, const AeNativeDef*> nativeFns;    // "io.readFile" 与 "readFile"
    vector<aehost::LoadedModule>     nativeModules;
    struct NativeRoots {
        vector<Value> args;                       // 传给本层原生函数的参数
        vector<GcString*> strings;       // 本层原生调用期间借出的字符串
        vector<Table*> tables;         // 本层原生调用期间借出的表
        vector<Closure*> funcs;        // 本层原生调用期间借出的闭包
    };
    vector<NativeRoots> nativeRoots;              // 嵌套原生调用的完整根栈
    int    bcMinor = 6;                           // 字节码次版本（决定 FUNC_DEF 头有没有上值表）
    string pendingNativeError;                    // raise() 设置，返回后转成运行错误
    string nativeCallError;                       // ABI v4：func_call 失败原因（借用给库）
    vector<Closure*> funcHandles;      // ABI v4：库持有的闭包句柄（下标+1 = handle）
    string nativeLastError;                       // set_error() 设置，lastError() 读取

    // 分代 + 增量的非移动式环收集器
    // shared_ptr 继续负责普通对象的即时析构；GC 只处理「环」——找出不可达对象
    // 并清空其出边，让引用计数归零。在此之上叠加三层改进：
    // · 分代：minor 只扫描并回收年轻对象（age < GC_PROMOTE_AGE），
    // 「老 → 年轻」的边由 remembered set 覆盖，避免每次都全堆扫描
    // · 增量：三色标记切成小步，在字节码安全点推进；写屏障保证标记期间
    // 被改写的对象退回灰色重新扫描，因此标记可以跨指令暂停
    // · 弱表：值不参与标记，标记结束后清除指向已死对象的条目
    enum class GcPhase { Idle, MarkRoots, Marking, Weak, Sweep };
    static const size_t kMinorBeforeMajor = 8;   // 每 8 次 minor 做一次 major

    vector<Table*>   gcTables;      // 全部表的弱登记
    vector<Closure*> gcClosures;    // 全部闭包的弱登记

    vector<Table*>   gcTableSet;   // 本轮快照：保证收集期间对象地址稳定
    vector<Closure*> gcClosureSet;
    vector<Table*>   gcGrayTables;            // 灰色工作队列
    vector<Closure*> gcGrayClosures;
    size_t gcGrayTableAt = 0, gcGrayClosureAt = 0;

    vector<Table*>   gcRememberedTables;      // 分代：老对象可能指向年轻对象
    vector<Closure*> gcRememberedClosures;
    vector<Upvalue*> gcRememberedUps;
    vector<Upvalue*>  gcUpvalues;      // 全部上值（GC 拥有，裸指针）
    vector<GcString*> gcStrings;      // 全部字符串（GC 拥有，裸指针）// 被改写过的上值（其值可能年轻）
    vector<Table*>   gcNextRemTables;         // 下一轮的 remembered
    vector<Closure*> gcNextRemClosures;

    GcPhase gcPhase = GcPhase::Idle;
    bool    gcIsMajor = false;
    bool    gcGenerational = true;
    bool    gcIncremental  = true;
    bool    gcStress = false;
    bool    gcStats  = false;
    size_t  gcAllocationDebt = 0;
    // GC 暂停计数：内置函数把值放在局部 vector里（不是 GC 根），
    // 一旦内部触发 execLoop（比如排序回调比较器），GC 就会回收它们 → 悬垂。
    // 短操作期间暂停 GC 是简单可靠的解法。
    int     gcPause = 0;
    // 字符串这类按体积分配的对象，光数个数会让 GC 反应太慢：
    // 循环里拼一个大字符串时，几十个中间结果就能吃掉几百 MB 才触发一次回收。
    // 所以另外按字节记一笔债，达到阈值就收集（与个数阈值是「或」的关系）。
    size_t  gcByteDebt = 0;
    static const size_t kByteThreshold = 8u << 20;   // 8 MB
    size_t  gcThreshold = 256;
    size_t  gcMinorSinceMajor = 0;
    size_t  gcStepBudget = 48;               // 每个安全点推进多少个工作单元
    size_t  gcMarkWork = 0;                  // 本轮已处理的灰色对象数（兜底用）
    size_t  gcCollections = 0, gcMinorCount = 0, gcMajorCount = 0;
    size_t  gcReclaimed = 0, gcSteps = 0, gcBarriers = 0, gcRescans = 0;
    size_t  gcLiveYoung = 0, gcLiveOld = 0, gcLastReclaimed = 0;

    static bool envFlagOn(const char* name, bool defOn) {
        const char* v = std::getenv(name);
        if (!v || !*v) return defOn;
        return !(std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0);
    }

    VM* gcPrevVM = nullptr;                 // 支持嵌套 VM（内置 run）：恢复外层全局指针

    VM() {
        gcPrevVM = g_gcVM; g_gcVM = this;
        gcStress = envFlagOn("AE_GC_STRESS", false);
        gcStats  = envFlagOn("AE_GC_STATS",  false);
        gcGenerational = envFlagOn("AE_GC_GENERATIONAL", true);
        gcIncremental  = envFlagOn("AE_GC_INCREMENTAL",  true);
        // 压力模式：每个安全点只推进一步，且分配一次就想收集 —— 最大程度打断标记
        if (gcStress) { gcThreshold = 1; gcStepBudget = 1; }
    }

    ~VM() {
        if (g_gcVM == this) g_gcVM = gcPrevVM;
        if (gcStats)
            cerr << "[gc] collections=" << gcCollections
                 << " minor=" << gcMinorCount << " major=" << gcMajorCount
                 << " reclaimed=" << gcReclaimed
                 << " steps=" << gcSteps << " barriers=" << gcBarriers
                 << " rescans=" << gcRescans << "\n";
        // 对象现在由 GC 显式拥有：退出时统一释放（引用计数已不复存在）。
        // 闭包先清空上值指针，避免析构时访问已删除的上值。
        for (Closure* c : gcClosures) if (c) c->ups.clear();
        for (Upvalue* u : gcUpvalues) delete u;
        for (GcString* x : gcStrings) delete x;
        for (Table* t : gcTables)     delete t;
        for (Closure* c : gcClosures) delete c;
        for (Coroutine* c : gcCoroutines) delete c;
        gcCoroutines.clear();
        gcUpvalues.clear(); gcStrings.clear(); gcTables.clear(); gcClosures.clear();
        breakAllManagedCycles();
    }

    Table* allocTable() {
        Table* value = new Table();
        gcTables.push_back(value);
        gcAllocationDebt++;
        // 收集进行中分配的对象该怎么上色，取决于当前处于哪一阶段：
        // · MarkRoots / Marking：标灰入队，让它的出边被扫描；同时进本轮快照
        // （队列里存的是裸指针，必须保证收集期间它不会被析构）
        // · Weak / Sweep：标记已结束，再入队也没人扫，灰色还会被清扫误判成
        // "不可达" —— 必须直接标黑，本轮放它一马
        if (gcPhase == GcPhase::MarkRoots || gcPhase == GcPhase::Marking) {
            value->gc.color = GC_GRAY;
            gcGrayTables.push_back(value);
            gcTableSet.push_back(value);
        } else if (gcPhase != GcPhase::Idle) {
            value->gc.color = GC_BLACK;
        }
        return value;
    }

    GcString* allocString(std::string&& s) {
        GcString* value = new GcString(std::move(s));
        value->gc = GcHeader();
        gcStrings.push_back(value);
        gcAllocationDebt++;
        gcByteDebt += value->size() + sizeof(GcString);
        if (gcPhase != GcPhase::Idle) value->gc.color = GC_BLACK;
        return value;
    }
    GcString* allocString(const std::string& s) {
        GcString* value = new GcString();
        *value = s;                       // 继承 std::string，赋值即可
        value->gc = GcHeader();
        gcStrings.push_back(value);
        gcAllocationDebt++;
        gcByteDebt += value->size() + sizeof(GcString);
        // 收集期间分配：字符串无出边，直接标黑放本轮一马（与表/闭包同规则）
        if (gcPhase != GcPhase::Idle) value->gc.color = GC_BLACK;
        return value;
    }
    Closure* allocClosure() {
        Closure* value = new Closure();
        gcClosures.push_back(value);
        gcAllocationDebt++;
        if (gcPhase == GcPhase::MarkRoots || gcPhase == GcPhase::Marking) {
            value->gc.color = GC_GRAY;
            gcGrayClosures.push_back(value);
            gcClosureSet.push_back(value);
        } else if (gcPhase != GcPhase::Idle) {
            value->gc.color = GC_BLACK;
        }
        return value;
    }

    // 分代判定
    bool gcParticipates(const GcHeader& h) const {
        return gcIsMajor || !gcGenerational || h.age < GC_PROMOTE_AGE;
    }
    bool gcIsOld(const GcHeader& h) const {
        return gcGenerational && h.age >= GC_PROMOTE_AGE;
    }

    // 三色标记
    // 返回 true 表示「目标是本轮参与的年轻对象」（供 remembered 保留判断）
    bool gcShadeTable(Table* t) {
        if (!t) return false;
        if (!gcParticipates(t->gc)) return false;
        if (t->gc.color == GC_WHITE) { t->gc.color = GC_GRAY; gcGrayTables.push_back(t); }
        return true;
    }
    bool gcShadeClosure(Closure* c) {
        if (!c) return false;
        if (!gcParticipates(c->gc)) return false;
        if (c->gc.color == GC_WHITE) { c->gc.color = GC_GRAY; gcGrayClosures.push_back(c); }
        return true;
    }
    bool gcShadeValue(const Value& v) {
        if (v.kind == VAL_TABLE) return gcShadeTable(v.tab);
        if (v.kind == VAL_FUNC)  return gcShadeClosure(v.fn);
        if (v.kind == VAL_STR)   return gcShadeString(v.str);
        if (v.kind == VAL_CO)    return gcShadeCoroutine(v.co);
        return false;
    }
    // 字符串不可变、没有任何出边，标黑即可，不必进灰色队列
    bool gcShadeString(GcString* s) {
        if (!s) return false;
        if (!gcParticipates(s->gc)) return false;
        if (s->gc.color == GC_WHITE) s->gc.color = GC_BLACK;
        return true;
    }
    // 协程：对象本身 + 挂起时保存的整套执行状态都是出边。
    // 挂起的协程其 stack / callStack 只有从这里才能被扫到 —— 漏了就是悬垂指针。
    bool gcShadeCoroutine(Coroutine* c) {
        if (!c) return false;
        // 协程不参与分代：晋升成"老"之后若被 minor GC 跳过，
        // 它挂起栈里引用的年轻对象就会被回收 → 悬垂指针（实测必崩）。
        // 协程数量远少于表，每轮都扫的成本可以接受。
        // （下面的 sweep 也据此不让协程晋升。）
        if (c->gc.color == GC_BLACK) return true;
        c->gc.color = GC_BLACK;
        gcShadeValue(c->fn);
        if (!c->hasState) return true;             // 运行中：状态在 VM 成员里，已是根
        for (const Value& v : c->stack) gcShadeValue(v);
        for (CallFrame& fr : c->callStack) {
            gcShadeClosure(fr.closure);
            for (const Value& v : fr.locals) gcShadeValue(v);
            for (DeferRec& d : fr.defers) {
                gcShadeValue(d.callee);
                for (const Value& v : d.args) gcShadeValue(v);
            }
            for (auto& kv : fr.openUps) if (kv.second) gcShadeUpvalue(kv.second);
        }
        return true;
    }
    // 上值由 GC 表统一拥有（闭包只持有指针），出边是它当前持有的值
    bool gcShadeUpvalue(Upvalue* u) {
        if (!u) return false;
        if (!gcParticipates(u->gc)) return false;
        if (u->gc.color == GC_WHITE) { u->gc.color = GC_BLACK; gcShadeValue(u->get()); }
        return true;
    }

    // 扫描一个对象的出边；弱值表的值不参与标记
    bool gcScanTable(Table* t) {
        if (!t) return false;
        if (t->gc.weakValues) return false;
        bool hit = false;
        for (const Value& v : t->arr) if (gcShadeValue(v)) hit = true;
        // 哈希段：GC 扫描不在乎顺序，直接走槽序最快
        // 键也要标记！字符串键是裸 GcString*，不 shade 就会被回收成悬垂指针
        for (const HashMap::Slot& s : t->mapv) {
            if (s.state != HashMap::FULL) continue;
            if (s.key.kind == VAL_STR) gcShadeString(s.key.str);
            if (gcShadeValue(s.val)) hit = true;
        }
        return hit;
    }
    bool gcScanClosure(Closure* c) {
        if (!c) return false;
        bool hit = false;
        for (Upvalue* up : c->ups) if (up && gcShadeValue(up->get())) hit = true;
        return hit;
    }

    // remembered set 存的是裸指针，而对象可能在两轮收集之间被 shared_ptr 析构。
    // 每轮开始用本轮快照筛一遍：不在快照里的对象已经不存在，绝不能再解引用。
    void gcPruneRemembered() {
        unordered_set<const Table*> aliveTables;
        aliveTables.reserve(gcTableSet.size());
        for (Table* t : gcTableSet) aliveTables.insert(t);
        unordered_set<const Closure*> aliveClosures;
        aliveClosures.reserve(gcClosureSet.size());
        for (Closure* c : gcClosureSet) aliveClosures.insert(c);

        vector<Table*> keptTables;
        keptTables.reserve(gcRememberedTables.size());
        for (Table* t : gcRememberedTables)
            if (t && aliveTables.count(t)) keptTables.push_back(t);
        gcRememberedTables = std::move(keptTables);

        vector<Closure*> keptClosures;
        keptClosures.reserve(gcRememberedClosures.size());
        for (Closure* c : gcRememberedClosures)
            if (c && aliveClosures.count(c)) keptClosures.push_back(c);
        gcRememberedClosures = std::move(keptClosures);
    }

    // 一轮收集的开始
    void gcStart(bool major) {
        gcIsMajor = major;
        gcPhase = GcPhase::MarkRoots;
        gcTableSet.clear(); gcClosureSet.clear();
        gcTableSet.reserve(gcTables.size());
        gcClosureSet.reserve(gcClosures.size());
        for (Table* t : gcTables)     gcTableSet.push_back(t);
        for (Closure* c : gcClosures) gcClosureSet.push_back(c);
        for (Table* t : gcTableSet)     t->gc.color = GC_WHITE;
        for (Closure* c : gcClosureSet) c->gc.color = GC_WHITE;
        for (GcString* x : gcStrings)   if (!x->gc.immortal) x->gc.color = GC_WHITE;
        for (Upvalue* u : gcUpvalues)   u->gc.color = GC_WHITE;
        for (Coroutine* c : gcCoroutines) c->gc.color = GC_WHITE;
        gcGrayTables.clear(); gcGrayClosures.clear();
        gcGrayTableAt = gcGrayClosureAt = 0;
        gcNextRemTables.clear(); gcNextRemClosures.clear();
        gcPruneRemembered();     // 用本轮快照筛掉两轮之间已析构的裸指针
        gcLiveYoung = gcLiveOld = 0;
        gcLastReclaimed = 0;
        gcMarkWork = 0;
    }

    // AOT 影子栈：编译成 C++ 之后，脚本的局部变量落在C 栈上，GC 看不见。
    // 每个 AOT 函数入口把自己的局部槽区登记进来（RAII 在返回/异常时注销），
    // GC 扫描时把它们当根。没有这个，AOT 里的活对象会被当成垃圾回收。
    struct ShadowReg { Value* slots; size_t n; };
    std::vector<ShadowReg> shadowStack;
    void shadowPush(Value* s, size_t n) { shadowStack.push_back({s, n}); }
    void shadowPop()  { if (!shadowStack.empty()) shadowStack.pop_back(); }

    // AOT 上值盒子：被闭包捕获的变量活在堆上的 Upvalue 里，不在 S[] 中。
    // 盒子数组本身在 C 栈上，GC 看不见 → 也要登记成根。
    struct BoxReg { Upvalue** b; size_t n; };
    std::vector<BoxReg> shadowBoxes;
    void shadowPushBoxes(Upvalue** b, size_t n) { shadowBoxes.push_back({b, n}); }
    void shadowPopBoxes() { if (!shadowBoxes.empty()) shadowBoxes.pop_back(); }

    // AOT 帧链：C++ 调用栈不是 Ae 调用栈，报错时要自己维护一条才能显示
    // "在 xxx() xxx.ae:11:18" 这样的栈。RAII 在函数入口压、出口弹（含异常路径）。
    struct AotFrame { const char* fn; const char* file; int line; };
    std::vector<AotFrame> aotFrames;

    // 造一个"已关闭"的上值盒子（AOT 没有可指向的帧槽，一律用 closed 形态）
    Upvalue* allocBox() {
        Upvalue* u = new Upvalue();
        u->open = nullptr;
        u->closed = Value();
        return u;
    }

    void gcMarkRoots() {
        for (const Value& v : stack) gcShadeValue(v);
        for (const ShadowReg& r : shadowStack)
            for (size_t i = 0; i < r.n; i++) gcShadeValue(r.slots[i]);
        for (const BoxReg& r : shadowBoxes)
            for (size_t i = 0; i < r.n; i++) if (r.b[i]) gcShadeUpvalue(r.b[i]);
        // 协程的挂起调用链是C++ 指针链（caller → co 的 parent 之类），
        // GC 从 Ae 值可达性是看不到的：主协程 resume 了子协程之后，主协程的
        // 状态被搬到 Coroutine 对象里、VM 成员上只剩子协程的栈 —— 于是主协程
        // 就成了"不可达"，会被回收，切回去时读到已释放内存（实测必崩）。
        // 所以凡是持有挂起状态的协程一律无条件当根。
        // 代价：丢弃一个仍然挂起的协程会泄漏到进程结束 —— 用 coroutine.close 显式关。
        for (Coroutine* c : gcCoroutines)
            if (c->hasState || c == curCo) gcShadeCoroutine(c);
        for (const Value& v : globals) gcShadeValue(v);
        for (const NativeRoots& r : nativeRoots) {
            for (const Value& v : r.args) gcShadeValue(v);
            for (Table* t : r.tables) gcShadeTable(t);
            for (Closure* c : r.funcs) gcShadeClosure(c);
        }
        for (Closure* c : funcHandles) gcShadeClosure(c);
        for (const CallFrame& f : callStack) {
            for (const Value& v : f.locals) gcShadeValue(v);
            gcShadeClosure(f.closure);
            for (const auto& e : f.openUps) gcShadeUpvalue(e.second);
        }
        // 分代：remembered 里的老对象要扫出边以发现年轻对象。
        // major 时工作队列已覆盖全部对象，无需重复。
        if (gcGenerational && !gcIsMajor) {
            for (Table* t : gcRememberedTables)
                if (t && gcScanTable(t)) gcNextRemTables.push_back(t);
            for (Closure* c : gcRememberedClosures)
                if (c && gcScanClosure(c)) gcNextRemClosures.push_back(c);
            for (Upvalue* up : gcRememberedUps)
                if (up) gcShadeValue(up->get());
        }
    }

    // 推进一小步（增量）
    bool gcHasGray() const {
        return gcGrayTableAt < gcGrayTables.size() ||
               gcGrayClosureAt < gcGrayClosures.size();
    }

    // 一轮标记允许的工作总量；超了就退化为一次性跑完
    size_t gcMarkWorkLimit() const {
        return (gcTableSet.size() + gcClosureSet.size()) * 2 + 1024;
    }

    void gcProcessOneGray() {
        gcMarkWork++;
        if (gcGrayTableAt < gcGrayTables.size()) {
            Table* t = gcGrayTables[gcGrayTableAt++];
            t->gc.color = GC_BLACK;
            bool old = gcIsOld(t->gc);
            if (gcScanTable(t) && old) gcNextRemTables.push_back(t);
        } else {
            Closure* c = gcGrayClosures[gcGrayClosureAt++];
            c->gc.color = GC_BLACK;
            bool old = gcIsOld(c->gc);
            if (gcScanClosure(c) && old) gcNextRemClosures.push_back(c);
        }
    }

    void gcAdvance() {
        gcSteps++;
        switch (gcPhase) {
            case GcPhase::MarkRoots:
                gcMarkRoots();
                gcPhase = GcPhase::Marking;
                break;
            case GcPhase::Marking: {
                size_t budget = gcStepBudget;
                while (budget > 0 && gcHasGray()) { gcProcessOneGray(); budget--; }
                if (!gcHasGray()) {
                    gcPhase = GcPhase::Weak;
                } else if (gcMarkWork > gcMarkWorkLimit()) {
                    // 兜底：分配速度超过标记速度，工作队列会无界增长。
                    // 此时退化为一次性跑完剩余标记（一次长暂停换来收敛）。
                    while (gcHasGray()) gcProcessOneGray();
                    gcPhase = GcPhase::Weak;
                }
                break;
            }
            case GcPhase::Weak:
                gcClearWeakEntries();
                gcPhase = GcPhase::Sweep;
                break;
            case GcPhase::Sweep:
                gcSweep();
                gcFinish();
                break;
            default: break;
        }
    }

    // 字节码安全点：按需启动一轮，并推进一小步
    void gcStep() {
        if (gcPause > 0) return;      // 见 gcPause 的说明
        if (gcPhase == GcPhase::Idle) {
            if (gcAllocationDebt < gcThreshold && gcByteDebt < kByteThreshold) return;
            bool major = !gcGenerational || gcMinorSinceMajor >= kMinorBeforeMajor;
            if (major) gcMinorSinceMajor = 0; else gcMinorSinceMajor++;
            gcStart(major);
        }
        gcAdvance();
        // 关闭增量时一轮跑到底（便于对比与排错）
        if (!gcIncremental) { while (gcPhase != GcPhase::Idle) gcAdvance(); }
    }

    // 同步完成一轮完整收集（collect() 用）
    size_t collectGarbageSync() {
        while (gcPhase != GcPhase::Idle) gcAdvance();   // 先跑完可能进行中的一轮
        gcStart(true);
        while (gcPhase != GcPhase::Idle) gcAdvance();
        return gcLastReclaimed;
    }

    // 弱表：清除指向已死对象的条目
    // 「已死」= 本轮参与标记且未被标记。老对象在 minor 中不参与，视为存活。
    bool gcValueDead(const Value& v) const {
        if (v.kind == VAL_TABLE && v.tab) {
            if (!gcParticipates(v.tab->gc)) return false;
            return v.tab->gc.color != GC_BLACK;
        }
        if (v.kind == VAL_FUNC && v.fn) {
            if (!gcParticipates(v.fn->gc)) return false;
            return v.fn->gc.color != GC_BLACK;
        }
        return false;   // 标量不归 GC 管
    }

    void gcClearWeakEntries() {
        for (Table* t : gcTableSet) {
            if (!t->gc.weakValues) continue;
            for (size_t i = 0; i < t->arr.size(); i++)
                if (gcValueDead(t->arr[i])) t->arr[i] = Value();
            while (!t->arr.empty() && t->arr.back().kind == VAL_NULL) t->arr.pop_back();
            // 哈希段：按下标扫；eraseSlot 只置墓碑、不搬元素，所以下标不会失效
            for (size_t i = 0; i < t->mapv.slots.size(); i++) {
                HashMap::Slot& s = t->mapv.slots[i];
                if (s.state == HashMap::FULL && gcValueDead(s.val))
                    t->mapv.eraseSlot(&s);
            }
        }
    }

    // 清扫
    // NaN-boxing 后对象由 GC显式拥有：不再有引用计数兜底，
    // 所以这里必须 delete 掉不可达对象，并从注册表里摘掉（以前只需清出边）。
    // 四类对象（表 / 闭包 / 上值 / 字符串）逐一处理。
    void gcSweep() {
        size_t reclaimed = 0;

        // 活的按代龄晋升并计入统计，死的直接 delete
        auto sweepSet = [&](auto& registry, auto&& keepAlive) {
            size_t keep = 0;
            for (size_t i = 0; i < registry.size(); i++) {
                auto* obj = registry[i];
                if (!obj) continue;
                GcHeader& h = obj->gc;
                // 永久对象（常量池字符串）：不参与、不回收、保留黑色
                if (h.immortal) { h.color = GC_BLACK; gcLiveOld++; registry[keep++] = obj; continue; }
                if (!gcParticipates(h)) { gcLiveOld++; registry[keep++] = obj; continue; }
                if (h.color == GC_BLACK) {
                    if (h.age < 255) h.age++;
                    if (gcIsOld(h)) gcLiveOld++; else gcLiveYoung++;
                    registry[keep++] = obj;
                } else {
                    delete obj;              // 显式释放
                    reclaimed++;
                }
            }
            registry.resize(keep);
            (void)keepAlive;
        };

        // 表：晋升的老对象要登记 remembered（它可能指向年轻对象）
        {
            size_t keep = 0;
            for (size_t i = 0; i < gcTables.size(); i++) {
                Table* t = gcTables[i];
                GcHeader& h = t->gc;
                if (!gcParticipates(h)) { gcLiveOld++; gcTables[keep++] = t; continue; }
                if (h.color == GC_BLACK) {
                    if (h.age < 255) h.age++;
                    if (gcIsOld(h)) {
                        gcLiveOld++;
                        if (!h.remembered) { h.remembered = true; gcNextRemTables.push_back(t); }
                    } else gcLiveYoung++;
                    gcTables[keep++] = t;
                } else { delete t; reclaimed++; }
            }
            gcTables.resize(keep);
        }
        // 闭包
        {
            size_t keep = 0;
            for (size_t i = 0; i < gcClosures.size(); i++) {
                Closure* c = gcClosures[i];
                GcHeader& h = c->gc;
                if (!gcParticipates(h)) { gcLiveOld++; gcClosures[keep++] = c; continue; }
                if (h.color == GC_BLACK) {
                    if (h.age < 255) h.age++;
                    if (gcIsOld(h)) {
                        gcLiveOld++;
                        if (!h.remembered) { h.remembered = true; gcNextRemClosures.push_back(c); }
                    } else gcLiveYoung++;
                    gcClosures[keep++] = c;
                } else { delete c; reclaimed++; }   // 闭包只持有上值指针，不 delete 它们
            }
            gcClosures.resize(keep);
        }
        // 上值（由 GC 表统一拥有）
        sweepSet(gcUpvalues, nullptr);
        // 字符串：无出边，delete 即可
        sweepSet(gcStrings, nullptr);
        // 协程
        {
            size_t keep = 0;
            for (size_t i = 0; i < gcCoroutines.size(); i++) {
                Coroutine* c = gcCoroutines[i];
                // 不晋升（age 恒为 0），保证下一轮 minor GC 仍会扫它的挂起栈
                if (c->gc.color == GC_BLACK) { gcLiveYoung++; gcCoroutines[keep++] = c; }
                else { delete c; reclaimed++; }
            }
            gcCoroutines.resize(keep);
        }

        gcReclaimed += reclaimed;
        gcLastReclaimed = reclaimed;
    }

    void gcFinish() {
        gcCollections++;
        if (gcIsMajor) gcMajorCount++; else gcMinorCount++;
        gcAllocationDebt = 0;
        gcByteDebt = 0;
        gcThreshold = gcStress ? 1
                    : std::max<size_t>(256, (gcLiveYoung + gcLiveOld) * 2 + 32);

        // 重建 remembered：先清旧标记，再按本轮结果重新置位
        for (Table* t : gcRememberedTables)   if (t) t->gc.remembered = false;
        for (Closure* c : gcRememberedClosures) if (c) c->gc.remembered = false;
        gcRememberedTables = std::move(gcNextRemTables);
        gcRememberedClosures = std::move(gcNextRemClosures);
        for (Table* t : gcRememberedTables)   if (t) t->gc.remembered = true;
        for (Closure* c : gcRememberedClosures) if (c) c->gc.remembered = true;

        // 上值登记：只保留「当前值是年轻对象」的那些，避免无界增长
        vector<Upvalue*> keepUps;
        for (Upvalue* up : gcRememberedUps) {
            if (!up) continue;
            const Value& v = up->get();
            Table*   t = (v.kind == VAL_TABLE) ? v.tab : nullptr;
            Closure* c = (v.kind == VAL_FUNC)  ? v.fn  : nullptr;
            bool young = (t && !gcIsOld(t->gc)) || (c && !gcIsOld(c->gc));
            if (young) keepUps.push_back(up);
        }
        gcRememberedUps = std::move(keepUps);

        // 登记表瘦身
        // 死对象已在 gcSweep 里 delete 并从登记表摘掉，这里无需再瘦身

        gcTableSet.clear(); gcClosureSet.clear();
        gcGrayTables.clear(); gcGrayClosures.clear();
        gcGrayTableAt = gcGrayClosureAt = 0;
        gcPhase = GcPhase::Idle;
    }

    // 写屏障的宿主实现（由文件末尾的自由函数调用进来）
    void onBarrierObject(GcHeader& h, bool isTable, void* obj) {
        gcBarriers++;
        // 分代：老对象被改写 → 可能新建了「老 → 年轻」的边
        if (gcIsOld(h) && !h.remembered) {
            h.remembered = true;
            if (gcPhase == GcPhase::Idle) {
                if (isTable) gcRememberedTables.push_back((Table*)obj);
                else         gcRememberedClosures.push_back((Closure*)obj);
            } else {
                if (isTable) gcNextRemTables.push_back((Table*)obj);
                else         gcNextRemClosures.push_back((Closure*)obj);
            }
        }
        // 增量：黑对象被改写 → 退回灰色，稍后重新扫描它的出边
        if ((gcPhase == GcPhase::Marking || gcPhase == GcPhase::MarkRoots) &&
            h.color == GC_BLACK) {
            h.color = GC_GRAY;
            if (isTable) gcGrayTables.push_back((Table*)obj);
            else         gcGrayClosures.push_back((Closure*)obj);
            gcRescans++;
        }
    }

    void onBarrierValue(const Value& v) {
        gcBarriers++;
        // 根区域写入：标记进行中时新值必须被标灰，否则本轮会被误回收
        if (gcPhase == GcPhase::Marking || gcPhase == GcPhase::MarkRoots)
            gcShadeValue(v);
    }

    void onBarrierUpvalue(Upvalue* up) {
        if (!up) return;
        onBarrierValue(up->get());
        // 分代：这个上值被改写过，它的值可能是年轻对象，供 minor 扫描
        if (gcGenerational) gcRememberedUps.push_back(up);
    }

    // 兼容旧名：同步完成一轮完整收集（内部走分代 + 增量状态机）
    size_t collectGarbage() { return collectGarbageSync(); }

    // VM 销毁时不再区分根：先断开所有托管对象的出边，保证最后的环也能析构。
    void breakAllManagedCycles() {
        // NaN-boxing 后对象由 gcSweep / 析构显式 delete，这里不再需要拆环。
        // 保留为空函数只是为了兼容既有的调用点。
    }

    // AeValue ↔ 内部 Value 的转换（ABI 边界的唯一通道）
    AeValue toAeValue(const Value& v) {
        AeValue a; a.kind = (int)v.kind; a.i = 0; a.f = 0.0; a.p = nullptr;
        switch (v.kind) {
            case VAL_INT:
            case VAL_BOOL:   a.i = v.i; break;
            case VAL_DOUBLE: a.f = v.f; break;
            case VAL_STR:    a.p = v.str; break;
            case VAL_TABLE:  a.p = v.tab; break;
            case VAL_FUNC:   a.p = v.fn;  break;   // ABI v4：借用，库不该保存它
            case VAL_CO:     a.p = v.co;  break;
            default: break;
        }
        return a;
    }
    GcString* findNativeString(const string* wanted) {
        if (!wanted) return nullptr;
        for (auto frame = nativeRoots.rbegin(); frame != nativeRoots.rend(); ++frame) {
            for (const Value& value : frame->args)
                if (value.kind == VAL_STR && value.str == wanted) return value.str;
            for (GcString* value : frame->strings)
                if (value == wanted) return value;
        }
        return nullptr;
    }

    Table* findNativeTable(const Table* wanted) {
        if (!wanted) return nullptr;
        for (auto frame = nativeRoots.rbegin(); frame != nativeRoots.rend(); ++frame) {
            for (const Value& value : frame->args)
                if (value.kind == VAL_TABLE && value.tab == wanted) return value.tab;
            for (Table* value : frame->tables)
                if (value == wanted) return value;
        }
        return nullptr;
    }

    Value fromAeValue(const AeValue& a) {
        switch (a.kind) {
            case AE_FUNC:   throw runtime_error("原生库不能把函数值当参数回传（请用 func_retain 拿句柄）");
            case AE_INT:    return Value((int64_t)a.i);
            case AE_BOOL:   return Value(a.i != 0);
            case AE_FLOAT:  return Value((double)a.f);
            case AE_STRING: {
                GcString* value = findNativeString((const string*)a.p);
                if (value) return Value(aeAllocString(*value));
                throw runtime_error("原生库返回了非法的字符串引用");
            }
            case AE_TABLE: {
                Table* table = findNativeTable((const Table*)a.p);
                if (table) return Value::makeTable(table);
                throw runtime_error("原生库返回了非法的表引用");
            }
            default: return Value();
        }
    }
    Table* findPoolTable(const AeValue& a) {
        Table* table = findNativeTable((const Table*)a.p);
        return table;
    }

    // 原生库调用（供 AOT 复用；字节码 0x26 走同一套校验与错误码）
    Value callNativeValue(const std::string& key, Value* args, int argc) {
        auto it = nativeFns.find(key);
        if (it == nativeFns.end())
            throw runtime_error("原生函数未加载: " + key +
                "（请在 .ae 文件里写 imp <库名> 后重新编译）");
        const AeNativeDef* d = it->second;
        if (d->minArgs >= 0 && argc < d->minArgs)
            throw runtime_error(key + " 至少需要 " + to_string(d->minArgs) + " 个参数");
        if (d->maxArgs >= 0 && argc > d->maxArgs)
            throw runtime_error(key + " 最多接受 " + to_string(d->maxArgs) + " 个参数");
        std::vector<Value> av(argc);
        for (int i = 0; i < argc; i++) av[i] = args[i];
        std::vector<AeValue> aev(argc);
        for (int i = 0; i < argc; i++) aev[i] = toAeValue(av[i]);

        NativeRoots roots;
        roots.args = std::move(av);
        nativeRoots.push_back(std::move(roots));
        std::string parentPendingError = std::move(pendingNativeError);
        pendingNativeError.clear();

        size_t before = stack.size();
        Value out;
        try {
            int pushed = d->fn(reinterpret_cast<AeVM*>(this), argc, argc ? aev.data() : nullptr);
            if (!pendingNativeError.empty()) {
                std::string e = pendingNativeError;
                thrownCode = AE_E_NATIVE;
                throw runtime_error(e);
            }
            size_t actual = stack.size() - before;
            size_t produced = (pushed > 0) ? (size_t)pushed : 0;
            if (produced > actual) produced = actual;
            if (produced >= 1) out = stack[stack.size() - 1];
            stack.resize(before);          // 原生压进栈的值已取出，还原栈
        } catch (...) {
            nativeRoots.pop_back();
            pendingNativeError = std::move(parentPendingError);
            throw;
        }
        nativeRoots.pop_back();
        pendingNativeError = std::move(parentPendingError);
        return out;
    }

    // 加载 .aeo 里声明的原生库
    void loadNativeModules(const vector<string>& names) {
        for (const string& nm : names) {
            // ① 先查内置库（io / os / path：静态链进 ae，没有库文件也能用）
            aebuiltin::Module bm;
            if (aebuiltin::find(nm, &bm)) {
                bm.setApi(aeNativeApi());
                int n = 0;
                const AeNativeDef* defs = bm.getExports(&n);
                if (!defs || n <= 0) throw runtime_error("内置库 " + nm + " 没有导出任何函数");
                for (int i = 0; i < n; i++) {
                    string key = nm + "." + defs[i].name;
                    nativeFns[key] = &defs[i];
                    if (!nativeFns.count(defs[i].name)) nativeFns[defs[i].name] = &defs[i];
                }
                continue;
            }
            // ② 内置表里没有 → 回退到动态库（第三方扩展仍可用 .so / .dll 提供）
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

    // P2-12：%c 用得到
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
    // 出错时按需读取源码；模块编译后每个文件各自缓存一份
    const string& sourceTextOf(int idx) {
        auto it = srcCache.find(idx);
        if (it != srcCache.end()) return it->second;
        string nm = fileNameOf(idx);
        string text;
        // 用宽字符读：中文名的源文件也要能摘录出来（返回后当 UTF-8 用）
        vector<uint8_t> bytes;
        string real;
        if (aepath::resolvePath(nm, &real) && aepath::readFileBytes(real, &bytes)) {
            text.assign(bytes.begin(), bytes.end());
        } else if (!scriptDir.empty()) {
            string base = nm;
            size_t s = base.find_last_of("/\\");
            if (s != string::npos) base = base.substr(s + 1);
            if (aepath::resolvePath(scriptDir + "/" + base, &real) && aepath::readFileBytes(real, &bytes))
                text.assign(bytes.begin(), bytes.end());
        }
        return srcCache.emplace(idx, text).first->second;
    }
    const string& sourceText() { return sourceTextOf(0); }
    string funcNameOf(uint16_t fid) const {
        for (const auto& fn : funcNames)
            if (fn.first == fid) return fn.second;
        return "?";
    }
    // P1-1：运行期错误的调用栈回溯
    // 每帧显示的是「该帧当前挂起在哪一行」：
    // 最内层 → 出错指令所在行；其余 → 它调用下一层的那一行
    std::string stackTrace() {
        std::string s;
        size_t top = callStack.size();
        for (size_t i = top; i-- > 0; ) {
            const CallFrame& fr = callStack[i];
            size_t p = (i + 1 == top) ? instrPC : callStack[i + 1].returnPC;
            int ln = 0, cl = 1, fi = 0;
            posForPC(p, ln, cl, &fi);
            // 和主诊断一样按显示列报（中文算 2 列），别一处字节列一处显示列
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
    void push(const Value& v) { stack.push_back(v); gcBarrierValue(v); }

    // P0-7：转换失败不再静默返回 0，而是抛出可读的运行时错误
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

    // P0-5：环检测 + 深度上限。
    // 此前 `local t = {} t[1] = t prln(t)` 会让这里无限递归 →
    // C++ 栈溢出 → 进程以 0xC00000FD 直接崩溃。现在打印闭环标记。
    string toString(const Value& v) {
        vector<const Table*> path;
        return strOf(v, path);
    }

    string strOf(const Value& v, vector<const Table*>& path) {
        switch (v.kind) {
            case VAL_NULL:  return "null";
            case VAL_STR:   return v.str ? std::string(*v.str) : std::string();
            case VAL_BOOL:  return v.i ? "true" : "false";
            case VAL_INT:   return to_string(v.i);
            case VAL_FUNC:  return v.fn ? ("<func #" + to_string(v.fn->funcId) + ">") : "<func>";
            case VAL_CO:    return "<coroutine>";
            case VAL_TABLE: {
                const Table* t = v.tab;
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
                // 哈希段（ 走排序迭代：规范承诺表的字符串形式是按键序的）
                t->mapv.forEachSorted([&](const HashMap::Slot& hs) {
                    if (!s.empty() && s.back() != '{') s += ", ";
                    const TableKey& k = hs.key;
                    if (k.kind == VAL_STR)      s += "\"" + string(k.str ? k.str->c_str() : "") + "\"";
                    else if (k.kind == VAL_INT) s += to_string(k.i);
                    else if (k.kind == VAL_BOOL) s += (k.i ? "true" : "false");
                    else if (k.kind == VAL_DOUBLE) s += to_string(k.f);
                    s += " = " + strOf(hs.val, path);
                });
                s += "}";
                path.pop_back();
                return s;
            }
            case VAL_DOUBLE: {
                // P2-9：与 printValue 共用同一条最短往返规则
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

    // 动态算术 / 严格比较（运行期按值的实际类型分派）
    // 编译器只做"粗略"的静态类型猜测，语义一律以运行期类型为准，
    // 因此整数族指令与浮点族指令行为完全等价。
    static bool isNumeric(const Value& v) {
        // P0-4：bool 不是数值 —— 它有自己的 type()，且判等时与 int 分家，
        // 若还允许 true + 1 = 2，就会出现"能相加却不相等"的自相矛盾
        return v.kind == VAL_INT || v.kind == VAL_DOUBLE;
    }
    static void requireNumeric(const Value& v, const char* opName) {
        if (!isNumeric(v))
            throw runtime_error(string("运算符 ") + opName + " 不支持 " +
                                typeName(v.kind) + " 类型" +
                                (v.kind == VAL_BOOL ? "（bool 不参与算术，可先 int(x) 显式转换）" : ""));
    }

    // P0-6：整数回绕语义。C++ 有符号溢出是 UB，直接用 `a + b` 会被优化器
    // 按"溢出不可能发生"处理；这里用无符号中转实现确定的二进制补码回绕。
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

    // P2-1：位运算只接受 int（bool 不是数值；float 请显式 int(x)）
    static void requireInt(const Value& v, const char* opName) {
        if (v.kind != VAL_INT)
            throw runtime_error(string("位运算符 ") + opName + " 只支持 int，得到 " +
                                typeName(v.kind) +
                                (v.kind == VAL_DOUBLE ? "（float 请用 int(x) 显式转换）" : ""));
    }

    // 把值追加到结果串：字符串直接追加内容，其余走 toString
    void appendToStr(std::string& out, const Value& v) {
        if (v.kind == VAL_STR) { if (v.str) out += *v.str; return; }
        out += toString(v);
    }
    Value doAdd(const Value& l, const Value& r) {
        // 只要任一侧是字符串 → 拼接（任意值转字符串）
        if (l.kind == VAL_STR || r.kind == VAL_STR) {
            // 直接把两侧追加进结果串：toString(l) + toString(r) 会多造两份临时串，
            // 对长字符串是两次完整拷贝
            std::string out;
            // 预留容量：否则 out 一路倍增扩容，长串拼接要多付一倍的内存拷贝
            size_t cap = 0;
            if (l.kind == VAL_STR) cap += l.str ? l.str->size() : 0;
            if (r.kind == VAL_STR) cap += r.str ? r.str->size() : 0;
            if (cap) out.reserve(cap);
            appendToStr(out, l);
            appendToStr(out, r);
            return Value(std::move(out));
        }
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
        // P2-10：向下取整除法（-7 / 3 == -3），与 Python/Lua 一致
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
        // P2-10：结果与除数同号（正除数时总为非负），配合向下取整除法
        int64_t m = l.i % r.i;
        if (m != 0 && ((m < 0) != (r.i < 0))) m += r.i;
        return Value(m);
    }

    // 比较（整数族 0x10-0x15 与 浮点族 0x16-0x1B 共用）
    // P0-3 定案语义（判等与大小比较刻意不同）：
    // / != ：
    // · 与 null 比较永远合法 —— null 表示"无值"，只与自身相等（false / true）
    // · int 与 float 之间报类型错：两者是同一数值域的不同表示，
    // 静默判为不等是最容易踩的坑（写 1.0 或 float(x) 即可）
    // · 其余类型不同 → false / true（不同类型本来就不相等）
    // < <= > >= ：
    // · 类型不同 → 报类型错（大小比较必须有可比性）
    // · 同类型里 null / 表没有大小关系 → 报类型错
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

    // P1-5：把栈顶 produced 个值调整成恰好 want 个
    // want=255 表示"全部保留"（预留给将来的多值展开）
    void adjustTop(size_t produced, uint8_t want) {
        if (want == 255) return;
        if (produced > (size_t)want) {
            stack.resize(stack.size() - (produced - want));
        } else {
            for (size_t i = produced; i < (size_t)want; i++) stack.push_back(Value());
        }
    }

    // P2-11/12/14/16：内建工具
    vector<Value> takeArgs(uint8_t argc) {
        vector<Value> a(argc);
        for (uint8_t i = 0; i < argc; i++) a[argc - 1 - i] = pop();
        return a;
    }

    // 内置 run(path [, args])：在同一个进程里加载并执行另一个 .aeo
    // 语义：
    // · b 跑完（自然结束，或 b 里 exit(n)）就回到 a 继续跑；返回值 = b 的退出码
    // · b 有自己一整套 VM 状态（全局变量 / 函数 / 常量池）→ 和 a 完全隔离
    // · b 里未捕获的错误 → 在 a 这边抛一个可 try/catch 的错误，错误码沿用
    // b 的（E2002 之类），消息里带 b 的位置与调用栈
    // · 第 2 个参数（可选）是字符串数组 → b 里 os.args() / args 库能看到
    // · b 开的窗口 / 定时器在 b 结束、回到 a 之前被清掉（见 reapChild）
    // · 嵌套上限 32 层（a 调 b、b 又调回 a 时不至于无限递归）
    // 为什么不用新进程：那会多开一个控制台窗口，也拿不到"跑完就回来"的语义。
    vector<VM*> subVMs;                 // 不能安全销毁的子 VM（见 reapChild）由父 VM 持有
    int         subRunDepth = 0;        // 当前 run 的嵌套层数
    static const int kMaxRunDepth = 32;

    static bool fileReadable(const string& p) {
        string real;
        return aepath::resolvePath(p, &real);        // 路径编码两种都试（UTF-8 / 本机 ANSI）
    }
    static string dirOfFile(const string& p) {
        size_t s = p.find_last_of("/\\");
        return (s == string::npos) ? string() : p.substr(0, s);
    }
    static string joinPath(const string& d, const string& f) {
        if (d.empty()) return f;
        char last = d[d.size() - 1];
        if (last == '/' || last == '\\') return d + f;
        return d + "/" + f;
    }
    static bool pathHasExt(const string& p) {
        size_t s = p.find_last_of("/\\");
        string base = (s == string::npos) ? p : p.substr(s + 1);
        return base.find('.') != string::npos;
    }

    // 找 .aeo：先按原样，再按调用者所在文件的目录找；没写后缀就补 .aeo 再试一轮
    string resolveRunPath(const string& want, string* triedOut) {
        int ln = 0, cl = 1, fi = 0;
        posForPC(instrPC, ln, cl, &fi);
        string callerDir = dirOfFile(fileNameOf(fi));       // 写 run(...) 的那个 .ae / .m
        vector<string> bases;
        bases.push_back("");                                // 原样（相对当前工作目录）
        if (!callerDir.empty()) bases.push_back(callerDir);
        if (!scriptDir.empty() && scriptDir != callerDir) bases.push_back(scriptDir);
        for (const string& r : aepath::assetRoots())  bases.push_back(r);   // 只写文件名时去 assets 里找
        vector<string> names;
        names.push_back(want);
        if (!pathHasExt(want)) names.push_back(want + ".aeo");
        string tried;
        for (const string& n : names)
            for (const string& b : bases) {
                string cand = joinPath(b, n);
                if (!tried.empty()) tried += ", ";
                tried += cand;
                string real;
                if (aepath::resolvePath(cand, &real)) {   // 解析成真实存在的 UTF-8 路径
                    if (triedOut) *triedOut = tried;
                    return real;
                }
            }
        if (triedOut) *triedOut = tried;
        return string();
    }

    // 在 target 这个 VM 的上下文里调一个不带参数的原生函数（用来收尾 / 复位 g_vm）
    void callNativeNoArg(const char* key, VM* target) {
        auto it = target->nativeFns.find(key);
        if (it == target->nativeFns.end() || !it->second || !it->second->fn) return;
        target->nativeRoots.push_back(NativeRoots());
        string parentPendingError = std::move(target->pendingNativeError);
        target->pendingNativeError.clear();
        int pushed = 0;
        try {
            pushed = it->second->fn(reinterpret_cast<AeVM*>(target), 0, nullptr);
        } catch (...) {
            target->nativeRoots.pop_back();
            target->pendingNativeError = std::move(parentPendingError);
            throw;
        }
        target->nativeRoots.pop_back();
        target->pendingNativeError = std::move(parentPendingError);
        if (pushed > 0 && !target->stack.empty()) target->stack.pop_back();   // 返回值丢掉
    }

    // 子程序结束后的收尾。返回 true = 可以安全销毁子 VM。
    // ui 是进程级的：子程序开的窗口/定时器还挂在 ui.dll 里，而它们的回调句柄
    // 属于子 VM —— 子 VM 一销毁句柄就悬空，所以必须先 ui.closeAll()。
    bool reapChild(VM* child) {
        bool uiLoaded = false;
        for (const auto& kv : child->nativeFns)
            if (kv.first.size() > 3 && kv.first.compare(0, 3, "ui.") == 0) { uiLoaded = true; break; }
        if (!uiLoaded) return true;                             // 没碰 ui → 直接销毁即可
        auto it = child->nativeFns.find("ui.closeAll");
        if (it == child->nativeFns.end() || !it->second || !it->second->fn)
            return false;                                       // 老 ui.dll 没有 closeAll：宁可留着也不悬空
        callNativeNoArg("ui.closeAll", child);                  // 关窗口 + 取消定时器 + 释放句柄
        callNativeNoArg("ui.count", this);                      // 顺手把 ui 的 g_vm 挪回父 VM
        return true;
    }

    // 把子程序的错误整理成父程序能读懂的一条消息（父程序这边抛出去）
    string childErrorText(VM* child, const string& want, const string& what) {
        string msg = "run(\"" + want + "\") 里出错: " + what;
        int ln = 0, cl = 1, fi = 0;
        child->posForPC(child->instrPC, ln, cl, &fi);
        if (ln > 0)
            msg += "\n    出错位置: " + child->fileNameOf(fi) + ":" + to_string(ln) + ":" + to_string(cl);
        string tr = child->stackTrace();
        while (!tr.empty() && tr.back() == '\n') tr.pop_back();
        if (!tr.empty()) {
            msg += "\n    调用栈:";
            size_t p = 0;
            for (;;) {
                size_t q = tr.find('\n', p);
                string one = tr.substr(p, (q == string::npos ? tr.size() : q) - p);
                if (!one.empty()) msg += "\n      " + one;
                if (q == string::npos) break;
                p = q + 1;
            }
        }
        return msg;
    }

    int64_t builtinRun(const vector<Value>& a) {
        // ① 参数检查
        if (a.empty() || a.size() > 2) {
            thrownCode = AE_E_ARITY;
            throw runtime_error("run() 需要 1 个参数（另一个 .aeo 的路径），可选第 2 个参数是字符串数组（传给子程序）");
        }
        if (a[0].kind != VAL_STR || !a[0].str) {
            thrownCode = AE_E_TYPE;
            throw runtime_error(string("run() 的第 1 个参数需要字符串（.aeo 路径），得到 ") + typeName(a[0].kind));
        }
        string want = *a[0].str;
        vector<string> childArgs;
        if (a.size() == 2) {
            if (!a[1].isTable()) {
                thrownCode = AE_E_TYPE;
                throw runtime_error("run() 的第 2 个参数需要表（字符串数组），例如 run(\"b.aeo\", {\"--x\", \"1\"})");
            }
            int64_t n = (int64_t)a[1].tab->length();
            for (int64_t i = 1; i <= n; i++) {
                Value v;
                if (!a[1].tab->get(Value(i), v)) break;
                if (v.kind != VAL_STR || !v.str) {
                    thrownCode = AE_E_TYPE;
                    throw runtime_error("run() 的第 2 个参数里第 " + to_string(i) + " 项需要字符串，得到 " + typeName(v.kind));
                }
                childArgs.push_back(v.str ? *v.str : string());
            }
        }
        if (want.empty()) {
            thrownCode = AE_E_FILE;
            throw runtime_error("run() 的路径是空的");
        }
        if (want.size() > 3 && want.compare(want.size() - 3, 3, ".ae") == 0) {
            thrownCode = AE_E_FILE;
            throw runtime_error("run() 要的是编译好的字节码（.aeo），不是源码 " + want + " —— 先跑 aec " + want);
        }
        if (subRunDepth >= kMaxRunDepth) {
            thrownCode = AE_E_STACK_OVER;
            throw runtime_error("run() 嵌套太深（上限 " + to_string(kMaxRunDepth) + " 层）—— 是不是 a 调 b、b 又调回 a？");
        }
        // ② 找文件
        string tried;
        string path = resolveRunPath(want, &tried);
        if (path.empty()) {
            thrownCode = AE_E_FILE;
            throw runtime_error("run() 找不到 " + want + "（找过: " + tried + "）");
        }
        // ③ 开子 VM 跑；b 的世界和 a 完全隔离
        VM* child = new VM();
        child->maxCallDepth = maxCallDepth;
        child->scriptArgs   = childArgs;
        subRunDepth++;
        child->subRunDepth  = subRunDepth;          // 子程序里再 run 时继续累加，防 a→b→a 死循环
        int64_t code = 0;
        bool    childThrew = false;
        string  errCode, errText;
        try {
            child->load(path);                      // 打不开 / 字节码坏了都在这里抛
            child->run();
        } catch (const ExitException& ex) {
            code = ex.code;                         // b 里 exit(n)：不结束父程序，作为 run 的返回值
        } catch (const exception& e) {
            childThrew = true;
            // 码沿用子程序的（catch 里能对上号），消息里带上 b 的位置和栈
            errCode = child->thrownCode.empty() ? classifyRun(e.what()) : child->thrownCode;
            string what = e.what();
            // 更里层的 run 已经包装过同一条消息 → 原样往上传，别套成 32 层 "run(...) 里出错:"
            errText = (what.compare(0, 5, "run(\"") == 0) ? what : childErrorText(child, want, what);
        }
        subRunDepth--;
        bool safeToFree = reapChild(child);
        if (safeToFree) delete child; else subVMs.push_back(child);
        if (childThrew) {
            thrownCode = errCode;
            throw runtime_error(errText);
        }
        return code;
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
    static Table* asTable(const Value& v, const char* fn) {
        if (!v.isTable())
            throw runtime_error(string(fn) + " 需要表参数，得到 " + typeName(v.kind));
        return v.tab;
    }
    static Coroutine* asCo(const Value& v, const char* fn) {
        if (!v.isCo())
            throw runtime_error(string(fn) + " 需要协程参数，得到 " + typeName(v.kind));
        return v.co;
    }
    static void wantArgs(size_t got, size_t need, const char* fn) {
        if (got != need)
            throw runtime_error(string(fn) + " 需要 " + to_string(need) + " 个参数，得到 " + to_string(got));
    }

    // 深拷贝（环安全）
    Value deepCopy(const Value& v, map<const Table*, Value>& seen) {
        if (v.kind != VAL_TABLE) return v;      // 标量与字符串不可变，可直接共享
        const Table* t = v.tab;
        if (!t) return v;
        auto it = seen.find(t);
        if (it != seen.end()) return it->second;
        Table* nt = allocTable();
        Value nv = Value::makeTable(nt);
        seen[t] = nv;
        nt->arr.resize(t->arr.size());
        for (size_t i = 0; i < t->arr.size(); i++) nt->arr[i] = deepCopy(t->arr[i], seen);
        // 深拷贝不在乎顺序，走槽序即可
        for (const HashMap::Slot& hs : t->mapv)
            nt->mapv.set(hs.key, deepCopy(hs.val, seen));
        gcBarrierTable(nt);   // 新表刚填满：让进行中的标记重新扫描它的出边
        return nv;
    }
    // 深相等（环安全：已在比较中的对视为相等）
    bool deepEqual(const Value& a, const Value& b, set<pair<const Table*,const Table*>>& seen) {
        if (a.kind != b.kind) return false;
        if (a.kind != VAL_TABLE) return valueEquals(a, b);
        const Table* ta = a.tab;
        const Table* tb = b.tab;
        if (ta == tb) return true;
        if (!ta || !tb) return false;
        auto key = make_pair(ta, tb);
        if (seen.count(key)) return true;
        seen.insert(key);
        if (ta->arr.size() != tb->arr.size() || ta->mapv.size() != tb->mapv.size()) return false;
        for (size_t i = 0; i < ta->arr.size(); i++)
            if (!deepEqual(ta->arr[i], tb->arr[i], seen)) return false;
        for (const HashMap::Slot& hs : ta->mapv) {
            Value other;
            if (!tb->mapv.get(hs.key, other)) return false;
            if (!deepEqual(hs.val, other, seen)) return false;
        }
        return true;
    }
    // 表中非 null 条目总数（区别于 len() 只数连续前缀）
    static int64_t tableCount(const Table* t) {
        int64_t n = 0;
        for (const Value& v : t->arr) if (v.kind != VAL_NULL) n++;
        for (const HashMap::Slot& hs : t->mapv) if (hs.val.kind != VAL_NULL) n++;
        return n;
    }
    // P2-12：printf 风格格式化
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

    // 内置函数分派
    // 0=pr 1=inp 2=prln 3=len 4=str 5=int 6=float 7=type
    // 8=assert 9=error 10=exit
    // 11=substr 12=ord 13=chr 14=find 15=upper 16=lower 17=trim
    // 18=replace 19=split 20=join 21=repeat 22=format
    // 23=delete 24=insert 25=pop 26=append 27=count
    // 28=copy 29=deepcopy 30=equal 31=raise 32=run
    // 33=weak 34=collect 35=gcstats GC：弱值表 / 手动收集 / 统计
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
            if (!prompt.empty()) { cout << prompt << flush; }   // P2-28：提示真正输出
            string line;
            if (getline(cin, line)) push(Value(aeAllocString(line)));
            else                    push(Value());          // EOF → null
            produced = 1;
        } else if (funcId == 3) {                 // len(x)
            Value v = pop();
            int64_t len = 0;
            if (v.kind == VAL_TABLE)      len = (int64_t)v.tab->length();
            else if (v.kind == VAL_STR)   len = v.str ? utf8Length(*v.str) : 0;  // P1-12：按字符
            else if (v.kind == VAL_INT) {
                int64_t n = v.i; if (n < 0) n = -n;
                if (n == 0) len = 1; else { while (n > 0) { len++; n /= 10; } }
            }
            // P2-13：float / bool / null 不再静默返回 0
            else throw runtime_error(string("len() 不支持 ") + typeName(v.kind) +
                                     " 类型（仅支持 string / table / int 的位数）");
            push(Value(len));
            produced = 1;
        } else if (funcId == 4) {                 // str(x)
            push(Value(aeAllocString(toString(pop()))));
            produced = 1;
        } else if (funcId == 5) {                 // int(x)
            push(Value(toInt(pop())));
            produced = 1;
        } else if (funcId == 6) {                 // float(x)
            push(Value(toDouble(pop())));
            produced = 1;
        } else if (funcId == 7) {                 // type(x)
            Value v = pop();
            // 必须显式构造 std::string：const char* → bool 是标准转换，
            // 会优先于 const char* → std::string 的用户自定义转换，
            // 之前这里恒压入 true。
            push(Value(string(typeName(v.kind))));
            produced = 1;
        } else if (funcId == 8) {                 // P1-6：assert(cond [, msg])
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
        } else if (funcId == 9) {                 // P1-6：error(msg)
            Value v = argc > 0 ? pop() : Value();
            thrownCode = AE_E_USER_ERROR;
            throw runtime_error(argc > 0 ? toString(v) : "脚本主动报错");
        } else if (funcId == 31) {                // 错误捕获：raise(e) 重新抛出
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
        } else if (funcId == 10) {                // P1-6：exit([code])
            Value v = argc > 0 ? pop() : Value();
            int code = 0;
            if (argc > 0) code = (int)toInt(v);
            throw ExitException{code};
        } else if (funcId == 32) {                // run(path [, args])：跑另一个 .aeo
            vector<Value> a = takeArgs(argc);
            if (a.empty() || a.size() > 2)
                throw runtime_error("run() 需要 1 个参数（.aeo 路径），可选第 2 个参数是字符串数组（传给子程序）");
            push(Value((int64_t)builtinRun(a)));
            produced = 1;
        } else if (funcId == 33) {                // weak(t [, mode])：把表变成弱值表
            vector<Value> a = takeArgs(argc);
            if (a.size() < 1 || a.size() > 2)
                throw runtime_error("weak() 需要 1~2 个参数：weak(表) 或 weak(表, \"v\")");
            if (!a[0].isTable())
                throw runtime_error("weak() 的第一个参数必须是表");
            string mode = (a.size() > 1) ? asStr(a[1], "weak") : string("v");
            if (mode == "v" || mode == "values") {
                a[0].tab->gc.weakValues = true;
            } else if (mode == "n" || mode == "none") {
                a[0].tab->gc.weakValues = false;
            } else if (mode == "k" || mode == "keys") {
                throw runtime_error("weak() 暂不支持弱键：Ae 的表键只能是 int/float/bool/string，"
                                    "字符串不归 GC 管理，弱键没有实际效果");
            } else {
                throw runtime_error("weak() 的模式只支持 \"v\"（弱值）与 \"n\"（取消弱引用）");
            }
            gcBarrierTable(a[0].tab);       // 弱标记改变了它的遍历方式
            push(a[0]);
            produced = 1;
        } else if (funcId == 34) {                // collect()：同步完成一轮完整收集
            takeArgs(argc);
            push(Value((int64_t)collectGarbageSync()));
            produced = 1;
        } else if (funcId == 35) {                // gcstats()：GC 观测
            takeArgs(argc);
            Table* t = allocTable();
            t->set(Value(string("collections")), Value((int64_t)gcCollections));
            t->set(Value(string("minor")),       Value((int64_t)gcMinorCount));
            t->set(Value(string("major")),       Value((int64_t)gcMajorCount));
            t->set(Value(string("reclaimed")),   Value((int64_t)gcReclaimed));
            t->set(Value(string("young")),       Value((int64_t)gcLiveYoung));
            t->set(Value(string("old")),         Value((int64_t)gcLiveOld));
            t->set(Value(string("steps")),       Value((int64_t)gcSteps));
            t->set(Value(string("barriers")),    Value((int64_t)gcBarriers));
            t->set(Value(string("rescans")),     Value((int64_t)gcRescans));
            t->set(Value(string("remembered")),
                   Value((int64_t)(gcRememberedTables.size() + gcRememberedClosures.size())));
            t->set(Value(string("threshold")),   Value((int64_t)gcThreshold));
            push(Value::makeTable(t));
            produced = 1;
        } else if (funcId == 36 || funcId == 37) {   // sort(t[, cmp]) / sortBy(t, key)
            vector<Value> a = takeArgs(argc);
            const bool byKey = (funcId == 37);
            const char* fn = byKey ? "sortBy" : "sort";
            if (a.size() < 1 || a.size() > 2)
                throw runtime_error(string(fn) + " 需要 1 或 2 个参数，得到 " + to_string(a.size()));
            Table* t = asTable(a[0], fn);
            Closure* cmpFn = nullptr;
            Value    keyVal;                     // sortBy：预先造一次键，别在比较里反复分配
            if (byKey) {
                if (a.size() != 2) throw runtime_error("sortBy 需要 2 个参数：sortBy(t, \"字段\")");
                keyVal = Value(aeAllocString(asStr(a[1], "sortBy")));
            } else if (a.size() == 2) {
                if (!a[1].isFunc() || !a[1].fn)
                    throw runtime_error("sort 的第二个参数必须是函数（比较器），得到 " +
                                        string(typeName(a[1].kind)));
                cmpFn = a[1].fn;
            }

            // 只排数组段：哈希段是命名字段，排序它没有意义。
            // 拷到临时数组里排，避免比较器（可能触发 GC / 改表）与被排序的 arr 互相干扰。
            struct GcPauseGuard {
                int* p;
                explicit GcPauseGuard(int* q) : p(q) { ++*p; }
                ~GcPauseGuard() { --*p; }
            } gcGuard(&gcPause);      // 排序期间禁止 GC：tmp / a 都不是 GC 根

            vector<Value> tmp = t->arr;

            auto lessDefault = [&](const Value& x, const Value& y) {
                // null 一律沉到最后（sortBy 时缺字段很常见，不该直接报错）
                if (x.kind == VAL_NULL) return false;
                if (y.kind == VAL_NULL) return true;
                bool xn = (x.kind == VAL_INT || x.kind == VAL_DOUBLE);
                bool yn = (y.kind == VAL_INT || y.kind == VAL_DOUBLE);
                if (xn && yn) return toDouble(x) < toDouble(y);
                if (x.kind == VAL_STR && y.kind == VAL_STR)
                    return (x.str ? *x.str : string()) < (y.str ? *y.str : string());
                if (x.kind == VAL_BOOL && y.kind == VAL_BOOL) return x.i < y.i;
                throw runtime_error(string(fn) + " 无法比较 " + typeName(x.kind) +
                                    " 与 " + typeName(y.kind) + "（表内元素类型要一致）");
            };

            if (cmpFn) {
                std::stable_sort(tmp.begin(), tmp.end(), [&](const Value& x, const Value& y) {
                    // 必须走 callValueFromNative：callUserFunction 只是压帧并改 pc，
                    // 真正执行要靠 execLoop，直接 pop 取不到返回值（会栈下溢）。
                    Value r;
                    string cerr;
                    Value fv = Value::makeFunc(cmpFn);
                    if (!callValueFromNative(fv, {x, y}, &r, &cerr))
                        throw runtime_error("sort 的比较器调用失败: " + cerr);
                    // 两种约定都支持：返回 bool（true = x 在前）/ 返回数字（<0 = x 在前）
                    if (r.kind == VAL_BOOL) return r.i != 0;
                    if (r.kind == VAL_INT || r.kind == VAL_DOUBLE) return toDouble(r) < 0.0;
                    throw runtime_error("sort 的比较器必须返回 bool 或数字，得到 " +
                                        string(typeName(r.kind)));
                });
            } else if (byKey) {
                auto fieldOf = [&](const Value& v, Value& out) {
                    if (!v.isTable())
                        throw runtime_error("sortBy 的元素必须是表，得到 " + string(typeName(v.kind)));
                    if (!v.tab->get(keyVal, out)) out = Value();   // 缺字段 → null
                };
                std::stable_sort(tmp.begin(), tmp.end(), [&](const Value& x, const Value& y) {
                    Value kx, ky; fieldOf(x, kx); fieldOf(y, ky);
                    return lessDefault(kx, ky);
                });
            } else {
                std::stable_sort(tmp.begin(), tmp.end(), lessDefault);
            }

            t->arr = std::move(tmp);
            push(Value::makeTable(t));            // 原地排序，返回表本身便于链式
            produced = 1;
        } else if (funcId >= 38 && funcId <= 42) {   // 协程
            vector<Value> a = takeArgs(argc);
            if (funcId == 38) {                       // coroutine.create(fn)
                if (a.size() != 1) throw runtime_error("coroutine.create 需要 1 个参数（函数）");
                if (!a[0].isFunc() || !a[0].fn)
                    throw runtime_error("coroutine.create 的参数必须是函数，得到 " +
                                        string(typeName(a[0].kind)));
                push(Value::makeCo(newCoroutine(a[0])));
                produced = 1;
            } else if (funcId == 39) {               // coroutine.resume(co, ...)
                if (a.empty()) throw runtime_error("coroutine.resume 至少需要 1 个参数（协程）");
                Coroutine* co = asCo(a[0], "coroutine.resume");
                vector<Value> cargs(a.begin() + 1, a.end());
                vector<Value> out;
                string err;
                bool ok = resumeCoroutine(co, std::move(cargs), out, err);
                push(Value(ok));
                if (ok) { for (const Value& v : out) push(v); }
                else    { push(Value(aeAllocString(err))); }
                produced = 1 + (ok ? out.size() : (size_t)1);
            } else if (funcId == 40) {               // coroutine.yield(...)
                // 只能在协程里调用：主执行流没有"上层"可以交回控制权
                if (!curCo || curCo->isMain || !curCo->parent)
                    throw runtime_error("coroutine.yield 只能在协程里调用");
                if (curCo) curCo->yieldWant = want;      // resume 传回的值要按这个数补齐
                yieldValues = std::move(a);
                throw YieldSignal{};
            } else if (funcId == 41) {               // coroutine.status(co)
                if (a.size() != 1) throw runtime_error("coroutine.status 需要 1 个参数");
                Coroutine* co = asCo(a[0], "coroutine.status");
                const char* nm = "dead";
                if (co == curCo) nm = "running";
                else if (co->status == Coroutine::SUSPENDED) nm = "suspended";
                push(Value(aeAllocString(string(nm))));
                produced = 1;
            } else {                                  // coroutine.close(co)
                if (a.size() != 1) throw runtime_error("coroutine.close 需要 1 个参数");
                Coroutine* co = asCo(a[0], "coroutine.close");
                if (co->status == Coroutine::DEAD) { push(Value(true)); produced = 1; }
                else if (co == curCo) { push(Value(false)); produced = 1; }   // 不能关自己
                else {
                    // 加载进来，把每一帧的 defer 反序跑掉，再废弃状态
                    Coroutine* caller = curCo;
                    saveExecState(caller);
                    loadExecState(co);
                    try {
                        for (size_t i = callStack.size(); i-- > 0; ) runDefersFor(i, 0);
                    } catch (...) { /* 清理阶段的错误不该再掀出去 */ }
                    stack.clear(); callStack.clear(); handlers.clear();
                    currentLocals = nullptr;
                    saveExecState(co);
                    co->status = Coroutine::DEAD;
                    co->hasState = false;
                    co->stack.clear(); co->callStack.clear(); co->handlers.clear();
                    loadExecState(caller);
                    curCo = caller;
                    push(Value(true));
                    produced = 1;
                }
            }
        } else if (funcId == 22) {                // P2-12：format(fmt, ...)
            vector<Value> a = takeArgs(argc);
            if (a.empty()) throw runtime_error("format() 需要至少 1 个参数");
            string fmt = asStr(a[0], "format");
            vector<Value> rest(a.begin() + 1, a.end());
            push(Value(aeAllocString(formatString(fmt, rest))));
            produced = 1;
        } else if (funcId >= 11 && funcId <= 21) {   // P2-11：字符串内建
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
                    push(Value(aeAllocString(utf8Encode((unsigned)asInt(a[0], "chr")))));
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
                    Table* t = allocTable();
                    size_t p = 0, q;
                    while ((q = s.find(sep, p)) != string::npos) {
                        t->arr.push_back(Value(aeAllocString(s.substr(p, q - p))));
                        p = q + sep.size();
                    }
                    t->arr.push_back(Value(aeAllocString(s.substr(p))));
                    gcBarrierTable(t);   // 新表刚填满：让进行中的标记重新扫描
                    push(Value::makeTable(t));
                    produced = 1;
                    break;
                }
                case 20: {   // join(t, sep)
                    wantArgs(a.size(), 2, "join");
                    Table* t = asTable(a[0], "join");
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
                push(Value(aeAllocString(r)));
                produced = 1;
            }
        } else if (funcId >= 23 && funcId <= 27) {   // P2-14：表原语
            vector<Value> a = takeArgs(argc);
            if (funcId == 23) {                      // delete(t, k) → bool
                wantArgs(a.size(), 2, "delete");
                Table* t = asTable(a[0], "delete");
                const Value& k = a[1];
                bool found = false;
                if (k.kind == VAL_INT && k.i >= 1 && k.i <= (int64_t)t->arr.size() &&
                    t->arr[(size_t)k.i - 1].kind != VAL_NULL) {
                    t->arr[(size_t)k.i - 1] = Value();
                    while (!t->arr.empty() && t->arr.back().kind == VAL_NULL) t->arr.pop_back();
                    found = true;
                } else {
                    if (t->mapv.erase(toKey(k))) found = true;
                }
                push(Value(found));
                produced = 1;
            } else if (funcId == 24) {               // insert(t, pos, v) → 新长度
                wantArgs(a.size(), 3, "insert");
                Table* t = asTable(a[0], "insert");
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
                Table* t = asTable(a[0], "pop");
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
                Table* t = asTable(a[0], "append");
                int64_t n = (int64_t)t->length() + 1;
                t->mapv.erase(toKey(Value(n)));
                t->set(Value(n), a[1]);
                push(Value(n));
                produced = 1;
            } else {                                 // count(t) → 非 null 条目数
                wantArgs(a.size(), 1, "count");
                push(Value(tableCount(asTable(a[0], "count"))));
                produced = 1;
            }
        } else if (funcId >= 28) {                   // P2-16：拷贝与深相等
            vector<Value> a = takeArgs(argc);
            if (funcId == 28) {                      // copy(t) —— 浅拷贝
                wantArgs(a.size(), 1, "copy");
                Table* t = asTable(a[0], "copy");
                Table* nt = allocTable();
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
        else if (v.kind == VAL_DOUBLE) cout << fmtDouble(v.f);   // P2-9：与 str() 一致
        else if (v.kind == VAL_CO)    cout << "<coroutine>";
        else cout << v.i;
    }

    // 预处理函数表
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
                // HALT 不再终止预扫描：现在的布局是
                // [入口 stub: CALL __script; HALT][FUNC_DEF ...]...
                // 若在此 break 就会漏掉后面所有函数定义
                continue;
            } else skipInstruction(op);
        }
        pc = savedPC;
    }

    void skipInstruction(uint8_t op) {
        switch (op) {
            case 0x01: case 0x04: case 0x05:
            case 0x30: case 0x31: case 0x2A: case 0x2B:
            case 0x2D:   // DEFER u8(argc) u16(scope) → 3 字节
                pc += 3; break;
            case 0x2E:   // DEFER_NATIVE u16(const) u8(argc) u16(scope) → 5 字节
                pc += 5; break;
            case 0x2F:   // END_DEFER u16(scope) → 2 字节
                pc += 2; break;
            case 0x50:   // NEW_TABLE u16(count) → 3 字节
                pc += 2; break;
            case 0x02:   // P1-5：CALL u8(id) u8(argc) u8(want) → 3 字节操作数
                pc += 3; break;
            case 0x41:   // P1-5：CALL_FUNC u16(id) u8(argc) u8(want) → 4 字节操作数
                pc += 4; break;
            case 0x44:   // 1.7：CLOSURE u16(funcId) u8(nups) → 3 字节
                pc += 3; break;
            case 0x45:   // 1.7：GET_UPVAL u16(i) → 2 字节
            case 0x46:   // 1.7：SET_UPVAL u16(i)
            case 0x47:   // 1.7：CLOSE_SLOT u16(slot)
            case 0x48:   // 1.7：CALL_VALUE u8(argc) u8(want)
                pc += 2; break;
            case 0x42:   // P1-10：RET u8(n) → 1 字节
                pc += 1; break;
            case 0x55:   // P1-3：THROW_CONST u16(idx) → 2 字节
                pc += 2; break;
            case 0x26:   // 原生库调用 NATIVE u16 + u8 + u8 → 4 字节操作数
                pc += 4; break;
            case 0x32:   // DEFER_BUILTIN u8 + u8 + u16 → 4 字节操作数
                pc += 4; break;
            default: break;
        }
    }

    // 协程：执行状态的保存 / 恢复 / 切换
    // stack / callStack 都是 vector，切换用 move —— O(1) 指针交接，不是拷贝。
    // 所以几百个协程也只在活跃时才占内存。
    void saveExecState(Coroutine* dst) {
        if (!dst) return;
        dst->stack     = std::move(stack);
        dst->callStack = std::move(callStack);
        dst->handlers  = std::move(handlers);
        dst->pc = pc; dst->instrPC = instrPC;
        dst->hasState = true;
    }
    void loadExecState(Coroutine* src) {
        stack     = std::move(src->stack);
        callStack = std::move(src->callStack);
        handlers  = std::move(src->handlers);
        pc = src->pc; instrPC = src->instrPC;
        src->hasState = false;
        // 裸指针雷区：currentLocals 指向 callStack.back().locals，
        // 切换后 callStack 整份都换了，必须按新帧重算，否则写进已失效的内存。
        currentLocals = callStack.empty() ? nullptr : &(callStack.back().locals);
    }

    Coroutine* newCoroutine(const Value& fn, bool isMain = false) {
        Coroutine* co = new Coroutine();
        co->gc = GcHeader();
        co->fn = fn;
        co->isMain = isMain;
        if (isMain) co->started = true;
        gcCoroutines.push_back(co);
        gcAllocationDebt++;
        if (gcPhase != GcPhase::Idle) co->gc.color = GC_BLACK;
        return co;
    }

    // resume 一个协程。成功返回 true，out 是 yield 出来（或函数返回）的值。
    bool resumeCoroutine(Coroutine* co, vector<Value> args, vector<Value>& out, string& err) {
        if (!co)                          { err = "coroutine.resume 需要一个协程"; return false; }
        if (co->status == Coroutine::DEAD)    { err = "不能 resume 已结束的协程"; return false; }
        if (co->status == Coroutine::RUNNING) { err = "不能 resume 正在运行的协程"; return false; }

        Coroutine* caller = curCo;
        saveExecState(caller);            // 先把主执行流（或上层协程）挂起来
        loadExecState(co);                // 再切进目标协程
        curCo = co;
        co->parent = caller;
        co->status = Coroutine::RUNNING;

        bool ok = true, yielded = false;
        yieldValues.clear();
        try {
            if (!co->started) {
                co->started = true;
                if (!co->fn.isFunc() || !co->fn.fn)
                    throw runtime_error("coroutine.create 的参数必须是函数");
                for (const Value& a : args) push(a);
                callUserFunction(co->fn.fn->funcId, (uint8_t)args.size(), 255, co->fn.fn);
                // 协程的第一帧没有真正的调用者：把 returnPC 指到 code 末尾，
                // 协程函数 return 后 pc 越界 → execLoop 自然结束（等价于 HALT）。
                if (!callStack.empty()) callStack.back().returnPC = code.size();
            } else {
                // resume 的参数 = 协程里那条 yield 的返回值
                for (const Value& a : args) push(a);
                adjustTop(args.size(), co->yieldWant);   // 按 yield 处的 want 补齐/裁剪
            }
            execLoop(0);                  // 跑到 yield 或协程结束
            yielded = yieldPending;
            yieldPending = false;
        } catch (const YieldSignal&) {
            yielded = true;               // 兜底（理论上走不到）
        } catch (const ExitException&) {
            // exit() 不能吞：状态复位后继续往外抛
            saveExecState(co);
            co->status = Coroutine::DEAD;
            loadExecState(caller);
            curCo = caller;
            throw;
        } catch (const exception& e) {
            ok = false; err = e.what();
        }

        saveExecState(co);                // 把协程停在哪儿记下来
        if (yielded && ok) {
            out = std::move(yieldValues);
            co->status = Coroutine::SUSPENDED;
        } else {
            co->status = Coroutine::DEAD;
            if (ok) out = std::move(co->stack);   // 协程函数 return 的值留在栈上
            else    { co->stack.clear(); co->deadReason = err; }
            co->stack.clear();
            co->callStack.clear();
            co->handlers.clear();
            co->hasState = false;
        }
        co->parent = nullptr;
        loadExecState(caller);            // 切回调用者
        curCo = caller;
        return ok;
    }

    // 主解释循环
    void run() {
        preScanFunctions();
        if (!curCo) curCo = newCoroutine(Value(), true);   // 主执行流也用协程对象表示
        pc = 0;
        execLoop(0);
    }

    // ABI v4：解释器主循环抽成独立方法，供"原生库回调 Ae 函数"嵌套执行。
    // stopDepth > 0 表示"跑到调用栈回落到 stopDepth 就返回"（回调已经返回了）。
    void execLoop(size_t stopDepth) {
        while (pc < code.size()) {
            if (stopDepth > 0 && callStack.size() <= stopDepth) return;
            gcStep();                        // GC 安全点：按需启动一轮并推进一小步
            instrPC = pc;                    // 记录当前指令起点，出错时用它定位
            uint8_t op = code[pc++];
            try {
            switch (op) {
                case 0x00: break; // NOP

                case 0x01: { // LOAD_CONST u16(idx) —— P1-2：下标校验
                    uint16_t idx = readU16(code, pc);
                    if (idx >= constPool.size())
                        throw runtime_error("常量池下标越界: #" + to_string(idx) +
                                            "（池中只有 " + to_string(constPool.size()) + " 项）");
                    const Const& c = constPool[idx];
                    if (c.tag == Const::UTF8)       push(Value(constString(idx)));
                    else if (c.tag == Const::INT32) push(Value((int64_t)c.i));
                    else if (c.tag == Const::INT64) push(Value((int64_t)c.i64));   // P0-2
                    else if (c.tag == Const::DOUBLE) push(Value(c.d));
                    else if (c.tag == Const::NULLV)  push(Value());   // null → VAL_NULL
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

                // 变量
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

                // 算术（ 运行期动态分派：整数/浮点自动提升）
                // 整数族(0x06-0x09) 与 浮点族(0x0A-0x0D) 的语义现在完全一致，
                // 均按两个操作数的实际类型决定结果，编译器选哪族都不影响正确性。
                case 0x06: case 0x0A: { Value r = pop(), l = pop(); push(doAdd(l, r)); break; }
                case 0x07: case 0x0B: { Value r = pop(), l = pop(); push(doSub(l, r)); break; }
                case 0x08: case 0x0C: { Value r = pop(), l = pop(); push(doMul(l, r)); break; }
                case 0x09: case 0x0D: { Value r = pop(), l = pop(); push(doDiv(l, r)); break; }
                case 0x0E: { Value v = pop(); push(Value((double)toDouble(v))); break; } // ITOD
                case 0x0F: { Value r = pop(), l = pop(); push(doMod(l, r)); break; }

                // 严格比较（整数族与浮点族统一实现）
                // 类型不同 → == / != / < / <= / > / >= 依次为 false/true/false...
                case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
                case 0x16: case 0x17: case 0x18: case 0x19: case 0x1A: case 0x1B: {
                    doCompare(op);
                    break;
                }

                // 独立 in 运算符
                case 0x29: {
                    Value c = pop(), n = pop();
                    push(doIn(n, c));
                    break;
                }

                // 布尔常量与逻辑非（P1-4）
                case 0x1C: { push(Value(true));  break; }
                case 0x1D: { push(Value(false)); break; }
                case 0x1E: { Value v = pop(); push(Value(isFalse(v))); break; }   // NOT：返回真 bool

                // P2-1 位运算 / P2-2 幂
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

                // 控制流
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

                // 局部变量
                case 0x30: { // LOCAL_STORE u16
                    uint16_t slot = readU16(code, pc);
                    if (!currentLocals) throw runtime_error("LOCAL_STORE 在函数外无效");
                    if (slot >= currentLocals->size())
                        throw runtime_error("LOCAL_STORE 局部槽越界: " + to_string(slot));
                    (*currentLocals)[slot] = pop();
                    gcBarrierValue((*currentLocals)[slot]);   // 根区域写入
                    break;
                }
                case 0x31: { // LOCAL_LOAD u16
                    uint16_t slot = readU16(code, pc);
                    if (!currentLocals) throw runtime_error("LOCAL_LOAD 在函数外无效");
                    if (slot >= currentLocals->size())
                        throw runtime_error("LOCAL_LOAD 局部槽越界: " + to_string(slot));
                    push((*currentLocals)[slot]);
                    break;
                }

                // 通用表（数组 / 字典合一）
                case 0x50: { // NEW_TABLE u16(count) —— 创建空表并压栈；count 仅作容量预留
                    uint16_t count = readU16(code, pc);
                    Table* tab = allocTable();
                    if (count > 0) tab->arr.reserve(count);
                    push(Value::makeTable(tab));
                    break;
                }
                case 0x51: { // TABLE_GET：栈 [tab, key] → value（未定义返回 null）
                    Value keyV = pop(), tabV = pop();
                    // P2-11：字符串支持 s[i]（1-based，按码点），越界返回 null
                    if (tabV.kind == VAL_STR) {
                        if (keyV.kind != VAL_INT)
                            throw runtime_error(string("字符串索引必须是 int（字符序号，1-based），得到 ") +
                                                typeName(keyV.kind));
                        int64_t n = (int64_t)utf8Length(tabV.str ? *tabV.str : string());
                        int64_t idx = keyV.i;
                        if (idx < 0) idx = idx + n + 1;                 // 负索引：-1 = 末位
                        string ch = utf8CharAt(tabV.str ? *tabV.str : string(), idx);
                        if (ch.empty() && idx > 0) push(Value());       // 越界 → null
                        else push(Value(ch));
                        break;
                    }
                    if (!tabV.isTable()) throw runtime_error(std::string("非表类型：'") + typeName(tabV.kind) +
                        "' 不能取键/字段（. 或 [] 只能用在表上）");
                    // 负索引只对整数键生效：t[-1] = 末位元素。
                    // 字符串键 "−1" 是普通键，不能被当成负索引（否则 "a"[-1] 就要去数数组段了）
                    Value useKey = keyV;
                    if (keyV.kind == VAL_INT && keyV.i < 0) {
                        int64_t n = (int64_t)lenOfTable(*tabV.tab);
                        useKey = Value(keyV.i + n + 1);
                    }
                    Value out;
                    if (!tabV.tab->get(useKey, out)) out = Value();   // null
                    push(out);
                    break;
                }
                case 0x52: { // TABLE_SET：栈 [tab, key, value] → 就地修改，结果（表）压回栈顶
                    Value val = pop(), keyV = pop(), tabV = pop();
                    if (!tabV.isTable()) throw runtime_error(std::string("非表类型：'") + typeName(tabV.kind) +
                        "' 不能按 key 赋值（t[k] = v 里的 t 必须是表）");
                    Value useKey = keyV;
                    if (keyV.kind == VAL_INT && keyV.i < 0) {
                        int64_t n = (int64_t)lenOfTable(*tabV.tab);
                        int64_t idx = keyV.i + n + 1;
                        // 写入仍要求落在 [1, n]：越界写会悄悄撑出空洞，不如直接报错
                        if (idx < 1)
                            throw runtime_error("表索引越界（负索引 " + to_string(keyV.i) +
                                                " 折算后是 " + to_string(idx) + "，1-based 最小为 1）");
                        useKey = Value(idx);
                    }
                    tabV.tab->set(useKey, val);
                    push(tabV);   // 表回到栈顶
                    break;
                }

                // 切片：栈 [容器, start, end, step] → 新容器（表切片给新表、字符串切片给新字符串）
                // 1-based 且闭区间：a[1:3] 取第 1..3 个。省略的段由 null 表示。
                case 0x2C: {
                    Value stepV = pop(), endV = pop(), startV = pop(), contV = pop();
                    push(doSlice(contV, startV, endV, stepV));
                    break;
                }

                // P1-3：取表的所有键（顺序确定：先数组段 1..n，再哈希段按键排序）
                // 值为 null 的槽不出现（与 Lua pairs 跳过 nil 一致）
                case 0x53: {
                    Value v = pop();
                    if (!v.isTable())
                        throw runtime_error(string("for-in 需要一个表，得到 ") + typeName(v.kind));
                    Table* out = allocTable();
                    const Table* t = v.tab;
                    for (size_t i = 0; i < t->arr.size(); i++)
                        if (t->arr[i].kind != VAL_NULL)
                            out->arr.push_back(Value((int64_t)(i + 1)));
                    // 走排序迭代：规范承诺"先数组段 1..n，再哈希段按键序"
                    t->mapv.forEachSorted([&](const HashMap::Slot& hs) {
                        if (hs.val.kind == VAL_NULL) return;
                        switch (hs.key.kind) {
                            case VAL_INT:    out->arr.push_back(Value((int64_t)hs.key.i)); break;
                            case VAL_BOOL:   out->arr.push_back(Value(hs.key.i != 0)); break;
                            case VAL_DOUBLE: out->arr.push_back(Value(hs.key.f)); break;
                            case VAL_STR:    out->arr.push_back(Value(hs.key.str ? *hs.key.str : string())); break;
                            default: break;
                        }
                    });
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
                case 0x27: { // PUSH_HANDLER u16(相对偏移)：注册错误处理器
                    uint16_t rel = readU16(code, pc);
                    Handler h;
                    h.frameDepth = callStack.size();
                    h.stackSize  = stack.size();
                    h.landing    = (size_t)((int64_t)pc + (int16_t)rel);
                    handlers.push_back(h);
                    break;
                }
                case 0x28: { // POP_HANDLER：注销最近注册的处理器
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

                    // 原生函数可以同步回调 Ae，回调里又可能进入另一个原生函数。
                    // 每层调用都有独立、可被 GC 枚举的根帧，借用指针不会被内层调用破坏。
                    NativeRoots roots;
                    roots.args = std::move(args);
                    nativeRoots.push_back(std::move(roots));
                    string parentPendingError = std::move(pendingNativeError);
                    pendingNativeError.clear();

                    size_t before = stack.size();
                    int pushed = 0;
                    try {
                        pushed = d->fn(reinterpret_cast<AeVM*>(this), (int)argc, argc ? av.data() : nullptr);
                        if (!pendingNativeError.empty()) {
                            string e = pendingNativeError;
                            thrownCode = AE_E_NATIVE;
                            throw runtime_error(e);
                        }
                        size_t produced = (pushed > 0) ? (size_t)pushed : 0;
                        size_t actual = stack.size() - before;
                        if (produced > actual) produced = actual;   // 防御：库虚报个数
                        adjustTop(produced, want);
                    } catch (...) {
                        nativeRoots.pop_back();
                        pendingNativeError = std::move(parentPendingError);
                        throw;
                    }
                    nativeRoots.pop_back();
                    pendingNativeError = std::move(parentPendingError);
                    break;
                }

                // 函数定义 / 调用 / 返回
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
                case 0x42: { // P1-10：RET u8(n) —— 把栈顶 n 个值作为返回值，
                             // 并彻底丢弃本帧遗留的所有操作数（回到 stackBase）
                    uint8_t n = readU8(code, pc);
                    if (callStack.empty()) return;
                    // 必须 move：否则每次返回都整份拷贝帧（含 locals 向量）→
                    // 等于每次调用多一次 malloc+拷贝+free。实测这是 fib 的最大热点之一。
                    CallFrame fr = std::move(callStack.back());
                    callStack.pop_back();
                    // 闭包：帧要没了，先把本帧打开的上值都关掉（值搬进堆上的闭包），
                    // 否则闭包会指向已释放的 locals —— 这是闭包逃逸时最容易出的悬空 bug
                    fr.closeUpvalues();
                    // 兜底：清掉属于已返回帧的处理器（正常路径由编译器补 POP_HANDLER）
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
                // 1.7：闭包
                case 0x44: { // CLOSURE u16(funcId) u8(nups)：按原型的上值表抓当前环境的变量
                    uint16_t funcId = readU16(code, pc);
                    uint8_t  nups   = readU8(code, pc);
                    if (funcId >= funcs.size())
                        throw runtime_error("CLOSURE 引用了未定义的函数 id=" + to_string(funcId));
                    const FuncDef& fd = funcs[funcId];
                    if (fd.ups.size() != nups)
                        throw runtime_error("CLOSURE 的上值个数与原型不符（字节码内部不一致）");
                    auto cl = allocClosure();
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
                    gcBarrierClosure(cl);  // 新闭包刚填好上值：让进行中的标记重新扫描
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
                    Upvalue* up = callStack.back().closure->ups[i];
                    up->get() = pop();
                    gcBarrierUpvalue(up);   // 上值被改写：shade 新值，并供 minor 扫描
                    break;
                }
                case 0x47: { // CLOSE_SLOT u16(slot)：把一个局部槽上的上值"关掉"
                             // 循环每轮末尾发这条，下一轮的闭包就会拿到全新的变量
                    uint16_t slot = readU16(code, pc);
                    if (!callStack.empty()) {
                        CallFrame& fr = callStack.back();
                        for (size_t k = 0; k < fr.openUps.size(); k++) {
                            if (fr.openUps[k].first == slot) {
                                Upvalue* up = fr.openUps[k].second;
                                up->close();
                                gcBarrierUpvalue(up);   // 同上值关闭
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
                    // AOT 闭包：直接调生成的 C++ 函数（不走字节码分派）
                    if (callee.fn->aotFn) {
                        Value rv[8];
                        int produced = callee.fn->aotFn(this, argc, argc ? args.data() : nullptr,
                                                        rv, callee.fn->ups.empty() ? nullptr
                                                                                   : callee.fn->ups.data());
                        for (int i = 0; i < produced && i < (int)want; i++) push(rv[i]);
                        for (int i = produced; i < (int)want; i++) push(Value());
                        break;
                    }
                    // 参数先压回栈（callUserFunction 自己从栈上取），再真正调用
                    for (const Value& a : args) push(a);
                    callUserFunction(callee.fn->funcId, argc, want, callee.fn);
                    break;
                }

                // defer / finally
                case 0x2D: { // DEFER u8(argc) u16(scope)：栈 [函数值, 参数...]
                    uint8_t argc = readU8(code, pc);
                    uint16_t scope = readU16(code, pc);
                    DeferRec d; d.native = false; d.argc = argc; d.scopeId = scope;
                    d.args.resize(argc);
                    for (int i = argc - 1; i >= 0; i--) d.args[i] = pop();
                    d.callee = pop();
                    if (callStack.empty()) break;
                    callStack.back().defers.push_back(std::move(d));
                    break;
                }
                case 0x2E: { // DEFER_NATIVE u16(const "mod.func") u8(argc) u16(scope)
                    uint16_t idx = readU16(code, pc);
                    uint8_t argc = readU8(code, pc);
                    uint16_t scope = readU16(code, pc);
                    DeferRec d; d.native = true; d.constIdx = idx; d.argc = argc; d.scopeId = scope;
                    d.args.resize(argc);
                    for (int i = argc - 1; i >= 0; i--) d.args[i] = pop();
                    if (callStack.empty()) break;
                    callStack.back().defers.push_back(std::move(d));
                    break;
                }
                case 0x32: { // DEFER_BUILTIN u8(bid) u8(argc) u16(scope)
                    uint8_t bid = readU8(code, pc);
                    uint8_t argc = readU8(code, pc);
                    uint16_t scope = readU16(code, pc);
                    DeferRec d; d.builtinId = bid; d.argc = argc; d.scopeId = scope;
                    d.args.resize(argc);
                    for (int i = argc - 1; i >= 0; i--) d.args[i] = pop();
                    if (callStack.empty()) break;
                    callStack.back().defers.push_back(std::move(d));
                    break;
                }
                case 0x2F: { // END_DEFER u16(scope)：退出该作用域，反序跑 defer
                    uint16_t scope = readU16(code, pc);
                    if (!callStack.empty()) runDefersFor(callStack.size() - 1, scope);
                    break;
                }
                case 0x56: { // THROW：抛出栈顶的错误值（finally 收尾后重抛用）
                    Value v = pop();
                    string msg;
                    if (v.isTable()) {
                        Value mv, cv;
                        if (v.tab->get(Value(string("msg")), mv))   msg = toString(mv);
                        if (v.tab->get(Value(string("code")), cv))  thrownCode = toString(cv);
                    } else {
                        msg = toString(v);
                    }
                    if (msg.empty()) msg = "重新抛出了一个错误";
                    if (thrownCode.empty()) thrownCode = AE_E_USER_ERROR;
                    throw runtime_error(msg);
                }

                default:
                    throw runtime_error("未知指令: 0x" + to_string(op));
            }
            } catch (const ExitException&) {
                throw;                                   // exit() 不可捕获
            } catch (const YieldSignal&) {
                // 协程 yield：状态留在原地（= 挂起点），主循环直接返回。
                // 不能直接把信号抛出去 —— callValueFromNative 的 catch 会吞掉它。
                yieldPending = true;
                return;
            } catch (const exception& e) {
                if (!handleRuntimeError(e.what())) throw; // 没处理器 → 继续往外抛
            }
        }
    }

    // 切片（供 AOT 复用；字节码 0x2C 也走这里）
    Value doSlice(const Value& contV, const Value& startV, const Value& endV, const Value& stepV) {
                
                int64_t len = 0;
                bool isStr = (contV.kind == VAL_STR);
                if (isStr) {
                    len = (int64_t)utf8Length(contV.str ? *contV.str : string());
                } else if (contV.isTable()) {
                    len = (int64_t)lenOfTable(*contV.tab);
                } else {
                    throw runtime_error("切片只能用在 table 或 string 上，得到 " +
                                        string(typeName(contV.kind)));
                }
                auto seg = [&](const Value& v, int64_t defVal) -> int64_t {
                    if (v.kind != VAL_INT) {
                        if (v.isNull()) return defVal;                 // 省略
                        throw runtime_error("切片的下标必须是 int（或省略），得到 " +
                                            string(typeName(v.kind)));
                    }
                    int64_t x = v.i;
                    if (x < 0) x = x + len + 1;                        // 负索引折算
                    return x;
                };
                int64_t step = 1;
                if (!stepV.isNull()) {
                    if (stepV.kind != VAL_INT)
                        throw runtime_error("切片的步长必须是 int，得到 " + string(typeName(stepV.kind)));
                    step = stepV.i;
                    if (step == 0) throw runtime_error("切片的步长不能为 0");
                }
                int64_t start, end;
                if (step > 0) { start = seg(startV, 1);              end = seg(endV, len); }
                else          { start = seg(startV, len);            end = seg(endV, 1);   }
                // 闭区间裁剪到 [1, len]
                if (start < 1) start = 1;
                if (end   < 1) end   = 1;
                if (start > len + 1) start = len + 1;
                if (end   > len) end = len;

                if (isStr) {
                    // 按码点取，索引 1-based 闭区间
                    string out;
                    if (step > 0) { for (int64_t i = start; i <= end && i >= 1 && i <= len; i += step) out += utf8CharAt(*contV.str, i); }
                    else          { for (int64_t i = start; i >= end && i >= 1 && i <= len; i += step) out += utf8CharAt(*contV.str, i); }
                    return Value(aeAllocString(out));
                } else {
                    Table* out = allocTable();
                    if (step > 0) { for (int64_t i = start; i <= end && i >= 1 && i <= len; i += step) out->arr.push_back(valueAt(*contV.tab, i)); }
                    else          { for (int64_t i = start; i >= end && i >= 1 && i <= len; i += step) out->arr.push_back(valueAt(*contV.tab, i)); }
                    return Value(out);
                }

    }
    // in 运算符（供 AOT 复用；字节码 0x29 也走这里）
    Value doIn(const Value& n, const Value& c) {

                if (c.isTable()) {
                    // 表同时是数组和字典，所以两条路都查：
                    // 1) 作为键存在（Python / JS 的 dict 语义）
                    // 2) 作为值出现在数组段里（列表语义，"a" in {"a","b"}）
                    // 两者居其一即为真 —— 单一类型只能定一条规则，取"任一命中"。
                    const Table* t = c.tab;
                    bool found = false;
                    Value tmp;
                    // null 不能作表键（Ae 的表键只允许 int/string/bool），
                    // 直接跳过键查找，否则会在 toKey() 里抛"该类型不可用作表键"
                    if (!n.isNull() && t->get(n, tmp) && !tmp.isNull()) found = true;
                    if (!found) {
                        for (size_t i = 0; i < t->arr.size() && !found; i++)
                            if (!t->arr[i].isNull() && valueEquals(t->arr[i], n)) found = true;
                    }
                    return Value(found);
                } else if (c.kind == VAL_STR) {
                    if (n.kind != VAL_STR)
                        throw runtime_error("in 的右边是 string 时左边也必须是 string（做子串判断），"
                                            "得到 " + string(typeName(n.kind)));
                    return Value(c.str->find(*n.str) != std::string::npos);
                } else {
                    throw runtime_error("in 的右边必须是 table 或 string，得到 " +
                                        string(typeName(c.kind)));
                }

    }
    // 构造脚本可见的错误表：{code, kind, msg, file, line, col, stack}
    Value buildErrorTable(const string& msg) {
        Table* t = allocTable();
        const char* code = thrownCode.empty() ? classifyRun(msg) : thrownCode.c_str();
        int line = 0, col = 1, fi = 0;
        posForPC(instrPC, line, col, &fi);
        // 列号换成显示列（中文算 2 列），跟打印出来的 ^ 位置一致
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
    // 出错时回退到最近的处理器；没有处理器就继续往外抛
    // defer
    // 执行一条 defer。 关键：defer 里再抛错一律吞掉—— 否则清理代码
    // 的错误会盖掉正在传播的原始错误（Java/C# 同理）。
    void runDefer(const DeferRec& d) {
        size_t savedStack = stack.size();
        std::vector<Handler> savedHandlers;
        savedHandlers.swap(handlers);          // 清空 → 内部错误不会被外层 catch 接走
        try {
            if (d.native) {
                if (d.constIdx >= constPool.size()) return;
                auto it = nativeFns.find(constPool[d.constIdx].s);
                if (it == nativeFns.end()) return;
                const AeNativeDef* def = it->second;
                vector<AeValue> av(d.argc);
                for (uint8_t i = 0; i < d.argc; i++) av[i] = toAeValue(d.args[i]);
                NativeRoots roots;
                roots.args = d.args;                       // 让 GC 看得见参数
                nativeRoots.push_back(std::move(roots));
                string savedErr = std::move(pendingNativeError);
                pendingNativeError.clear();
                try { def->fn(reinterpret_cast<AeVM*>(this), (int)d.argc, d.argc ? av.data() : nullptr); }
                catch (...) {}
                pendingNativeError = std::move(savedErr);
                nativeRoots.pop_back();
            } else if (d.builtinId != 0xFF) {
                // 内置函数（prln / len ...）：参数回栈后走和 CALL 一样的分派
                for (uint8_t i = 0; i < d.argc; i++) push(d.args[i]);
                invoke(d.builtinId, d.argc, 0);
            } else {
                if (!d.callee.isFunc() || !d.callee.fn) return;
                callValueFromNative(d.callee, d.args, nullptr, nullptr);   // 嵌套执行，错误写进 err 被忽略
            }
        } catch (...) {}
        handlers.swap(savedHandlers);
        if (stack.size() > savedStack) stack.resize(savedStack);
        currentLocals = callStack.empty() ? nullptr : &(callStack.back().locals);
    }

    // 退出作用域：把该帧里 scopeId >= 门槛的记录按反序跑完。
    // 必须先把记录拷出来再执行：runDefer 会往 callStack 压新帧，
    // vector 扩容会让帧的引用/指针失效（此前直接持有 CallFrame& 就是崩在这）。
    void runDefersFor(size_t frameIdx, uint16_t threshold) {
        if (frameIdx >= callStack.size()) return;
        vector<DeferRec> todo;
        {
            CallFrame& fr = callStack[frameIdx];
            // 只摘走本轮该跑的记录，其余（scopeId 更小 = 更外层）必须留着。
            // 此前是无条件 clear()，于是内层块退出会把还没到时机的外层 defer 一起丢掉
            // —— 典型症状：函数里既有 defer 又有 try/finally 时，defer 一次都不跑。
            vector<DeferRec> keep;
            keep.reserve(fr.defers.size());
            for (size_t i = fr.defers.size(); i-- > 0; ) {
                if (fr.defers[i].scopeId >= threshold) todo.push_back(fr.defers[i]);
                else keep.push_back(fr.defers[i]);
            }
            std::reverse(keep.begin(), keep.end());   // 上面是倒着扫的，翻回来保持注册顺序
            fr.defers.swap(keep);
        }
        for (const DeferRec& d : todo) {
            runDefer(d);
            if (frameIdx >= callStack.size()) break;     // 帧已经没了就别继续
        }
    }

    bool handleRuntimeError(const string& msg) {
        if (handlers.empty()) return false;
        Value err = buildErrorTable(msg);            // 先用出错现场的信息建表
        Handler h = handlers.back();
        handlers.pop_back();
        while (callStack.size() > h.frameDepth) {
            // defer：异常往外传播时，被退掉的每一帧都要跑完它的 defer（反序）
            runDefersFor(callStack.size() - 1, 0);
            callStack.back().closeUpvalues();        // 闭包：退掉的帧也要关上值
            callStack.pop_back();
        }
        currentLocals = callStack.empty() ? nullptr : &(callStack.back().locals);
        stack.resize(h.stackSize);
        push(err);                                   // catch 变量就是它
        pc = h.landing;
        return true;
    }

    // 1.7：取当前帧某个局部槽的"开放上值"。
    // 同一个槽被抓多次 → 共享同一个 Upvalue（所以两个闭包改的是同一个变量）。
    Upvalue* makeOpenUpvalue(uint16_t slot) {
        if (callStack.empty()) throw runtime_error("没有活动帧，无法捕获变量");
        CallFrame& fr = callStack.back();
        if (slot >= fr.locals.size())
            throw runtime_error("捕获了不存在的局部槽 " + to_string(slot) + "（字节码内部不一致）");
        for (auto& kv : fr.openUps)
            if (kv.first == slot) return kv.second;
        auto up = new Upvalue();
        // 注意：指向的是 locals 这块堆缓冲区里的元素，不是 CallFrame 对象本身。
        // vector<CallFrame> 扩容会搬 CallFrame，但搬不动 locals 的缓冲区，所以指针依旧有效。
        up->open = &fr.locals[slot];
        fr.openUps.push_back({slot, up});
        return up;
    }

    // 1.7：cl 非空 = 调用一个闭包（帧里带上上值表）；空 = 普通全局函数
    // ABI v4：从原生库回调一个 Ae 函数值。
    // 这是一个"嵌套执行"：外层解释器状态全部保存/恢复，回调内部可以
    // 继续调用原生库、可以用 try/catch、可以 raise；出错则返回 false 并把
    // 原因写进 err（不在这里决定是抛还是忽略 —— 交给库，也交给外层的 try/catch）。
    bool callValueFromNative(const Value& fn, const vector<Value>& args, Value* out, string* err) {
        if (!fn.isFunc() || !fn.fn) { if (err) *err = "call_value: 不是函数值"; return false; }
        if (args.size() > 255)      { if (err) *err = "call_value: 参数太多"; return false; }

        size_t savedPC = pc, savedInstrPC = instrPC;
        // (原来的 currentLocals 保存指针已改成按帧深度重取，见下方恢复处)
        size_t savedDepth = callStack.size();
        size_t savedStackTop = stack.size();
        string savedPending = pendingNativeError;
        vector<Handler> savedHandlers;               // 回调有自己的try 层
        savedHandlers.swap(handlers);
        bool ok = true;

        try {
            // AOT 生成的闭包没有字节码 funcId，直接走 C 函数指针
            if (fn.fn->aotFn) {
                std::vector<Value> av = args;
                Value rv[8];
                for (int i = 0; i < 8; i++) rv[i] = Value();
                int n = fn.fn->aotFn(this, (int)av.size(), av.empty() ? nullptr : av.data(), rv,
                                     fn.fn->ups.empty() ? nullptr : fn.fn->ups.data());
                if (out) *out = (n > 0) ? rv[0] : Value();
                handlers.swap(savedHandlers);
                return true;
            }
            for (const Value& a : args) push(a);
            callUserFunction(fn.fn->funcId, (uint8_t)args.size(), 1, fn.fn);
            execLoop(savedDepth);
            if (yieldPending) {           // C 栈没法 unwind，原生回调里不允许 yield
                yieldPending = false;
                ok = false;
                if (err) *err = "不能在原生库的回调里 coroutine.yield（把 yield 包在 Ae 函数里）";
            }
            Value r;
            if (stack.size() > savedStackTop) r = pop();
            if (out) *out = r;
        } catch (const ExitException&) {
            // exit() 不能被回调吞掉：恢复状态后一路抛出去，让程序真的结束
            while (callStack.size() > savedDepth) { callStack.back().closeUpvalues(); callStack.pop_back(); }
            stack.resize(savedStackTop);
            handlers.swap(savedHandlers);
            pc = savedPC; instrPC = savedInstrPC;
            // 不能直接恢复保存的指针：嵌套调用会让 callStack 扩容/搬迁，
            // 那个指针就悬空了（症状：回调返回后宿主函数的局部变量全被写进
            // 已释放内存，变量莫名变成 null/0）。按帧深度重新取。
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
        currentLocals = (savedDepth == 0) ? nullptr : &(callStack[savedDepth - 1].locals);   // 见上
        pendingNativeError = savedPending;
        return ok;
    }

    void callUserFunction(uint16_t funcId, uint8_t argc, uint8_t want,
                          Closure* cl = nullptr) {
        if (funcId >= funcs.size()) throw runtime_error("调用未定义的函数 id=" + to_string(funcId));
        const FuncDef& fd = funcs[funcId];
        // 默认参数（minor>=8）：静态调用已在编译期补全，argc 通常就等于 paramCount。
        // 动态调用（CALL_VALUE）编译期不知道目标是谁，只能在这里按默认值表补齐。
        // 变参：上限放开（argc 是 u8，天然 ≤ 255），只检查下界
        uint16_t maxArgs = fd.isVariadic() ? 255 : fd.paramCount;
        uint16_t minArgs = fd.defaultConsts.empty() ? fd.paramCount : fd.minParamCount();
        if (argc < minArgs || argc > maxArgs)
            throw runtime_error("函数参数数量不匹配: 期望 " + to_string(minArgs) + "~" +
                                to_string(maxArgs) + "，实际传入 " + to_string(argc));
        if (callStack.size() >= maxCallDepth)
            throw runtime_error("调用栈溢出（超过 " + to_string(maxCallDepth) +
                                " 层，可用 --max-depth 调整）");
        vector<Value> args(argc);
        for (int i = argc - 1; i >= 0; i--) args[i] = pop();

        CallFrame frame;
        frame.funcId    = funcId;
        frame.returnPC  = pc;
        frame.stackBase = stack.size();      // P1-10：本帧的操作数栈基址
        frame.want      = want;              // P1-5
        frame.closure   = std::move(cl);
        frame.locals.assign(fd.localCount, Value());
        // 固定参数位（不含变参那位）—— 逐个绑定，缺的用默认值补
        uint16_t fixed = fd.fixedParamCount();
        uint16_t n = (uint16_t)argc < fixed ? (uint16_t)argc : fixed;
        for (uint16_t i = 0; i < n; i++) frame.locals[i] = args[i];
        for (uint16_t i = (uint16_t)argc; i < fixed; i++) {
            if (i >= fd.defaultConsts.size()) break;
            uint16_t ci = fd.defaultConsts[i];
            if      (ci == 0xFFFF) break;                  // 必传位，不该到这
            else if (ci == 0xFFFE) frame.locals[i] = Value(true);
            else if (ci == 0xFFFD) frame.locals[i] = Value(false);
            else if (ci < constPool.size()) {
                const Const& c = constPool[ci];
                if      (c.tag == Const::UTF8)   frame.locals[i] = Value(aeAllocString(c.s));
                else if (c.tag == Const::INT32)  frame.locals[i] = Value((int64_t)c.i);
                else if (c.tag == Const::INT64)  frame.locals[i] = Value((int64_t)c.i64);
                else if (c.tag == Const::DOUBLE) frame.locals[i] = Value(c.d);
                else if (c.tag == Const::NULLV)  frame.locals[i] = Value();
            }
        }
        // 变参：多余的参数打包成表（下标从 1 开始），零个给空表而不是 null
        if (fd.isVariadic() && fd.variadicSlot < fd.localCount) {
            Table* rest = new Table();
            for (uint16_t i = fixed; i < (uint16_t)argc; i++)
                rest->set(Value((int64_t)(i - fixed + 1)), args[i]);
            frame.locals[fd.variadicSlot] = Value(rest);
        }

        callStack.push_back(std::move(frame));
        currentLocals = &(callStack.back().locals);
        pc = fd.entryPC;
    }

    // .aeo 加载
    // 读一个小节：u32 长度 + 字节
    static void readBlob(const vector<uint8_t>& b, size_t& p, string& out, const char* what) {
        if (p + 4 > b.size()) throw runtime_error(string(what) + " 长度字段截断");
        uint32_t n = readU32(b, p);
        if (n > b.size() - p) throw runtime_error(string(what) + " 数据越界: " + to_string(n));
        out.assign((const char*)(b.data() + p), n);
        p += n;
    }

    void load(const string& path) {
        // 路径可能是 UTF-8（脚本里 run() 传的）也可能是本机 ANSI（命令行传的）：
        // 先解析出真实存在的那个，同时拿到 UTF-8 形式（诊断里显示才不乱码）
        string real = path;
        // 用 resolveAssetPath：命令行上写个裸文件名也能在 assets 里找到对应 .aeo
        if (!aepath::resolveAssetPath(path, &real)) {
            if (!aepath::fileExists(path))
                throw LoadIOError("无法打开: " + path + "（源文件要先编译: aec 源文件.ae）");
        }
        vector<uint8_t> all;
        if (!aepath::readFileBytes(real, &all))
            throw LoadIOError("无法打开: " + real + "（源文件要先编译: aec 源文件.ae）");
        if (all.size() < 16) throw runtime_error("文件过短，不是有效的 .aeo 文件");

        size_t p = 0;
        if (all[p]!='A' || all[p+1]!='e' || all[p+2]!='B' || all[p+3]!='c')
            throw runtime_error("魔数不匹配");
        p += 4;
        // P1-2：版本必须校验（此前只是 `p += 2` 跳过，改成 99.42 也照跑）
        uint8_t major = readU8(all, p);
        uint8_t minor = readU8(all, p);
        if (major != 1)
            throw runtime_error("不支持的字节码主版本: " + to_string(major) + "（本 VM 支持 1.x）");
        if (minor > 8)
            throw runtime_error("字节码次版本过新: 1." + to_string(minor) + "（本 VM 支持到 1.7，请重新编译源码）");
        bcMinor = minor;      // 1.7 起 FUNC_DEF 头带上值描述表（闭包）
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
                // null 字面量：仅 tag 字节，无附加数据
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
                // P0-2：8 字节大端
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
        // P1-1/P1-2：v1.1 新增的调试信息段（次版本 0 的旧文件没有这些段）
        if (minor >= 1) {
            readBlob(all, p, srcName, "源文件名");
            if (minor >= 6) {                                  // 源文件表
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

        // 1.3：代码段之后的原生库导入表
        scriptDir = aehost::dirOf(real);       // 用解析后的 UTF-8 路径（中文目录也能找库）
        srcPath   = real;
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

        // P1-2：预扫描后的函数表必须自洽（每个函数体都要在代码段内）
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
// 运行期诊断渲染（分类表集中维护）
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
        // 坏 .aeo / 版本不对也归到「字节码损坏」，别报成 E2011 内部错误
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
        // 内建函数的参数类型/解析失败（以前落到 E2011「VM 内部错误」，把用户的错说成
        // 虚拟机的错）：asStr/asInt/asTable 与 int()/float() 的解析
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
// codeHint：抛出时已经定好的错误码（原生库 / assert / error / raise 表）
// 以前这里只用消息去猜，猜不中一律印成 E2011「VM 内部错误」——库参数写错、脚本
// 自己 raise 的错误也被说成内部错误，而 try/catch 里看到的却是真实码，两处对不上。
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

// 写屏障的宿主实现：转发给当前活动的 VM
// Table::set / 栈 push / 局部与全局写入 / 上值改写都会走到这里。
// 屏障在 GC 空闲时几乎只是一个分支判断，只有在标记进行中才真正干活。
VM* g_gcVM = nullptr;

void gcBarrierTable(Table* t) {
    if (g_gcVM && t) g_gcVM->onBarrierObject(t->gc, true, t);
}
void gcBarrierTableValue(Table* t, const Value& v) {
    if (!g_gcVM || !t) return;
    g_gcVM->onBarrierObject(t->gc, true, t);
    g_gcVM->onBarrierValue(v);          // 新值直接标灰，避免它在本轮被误回收
}
void gcBarrierClosure(Closure* c) {
    if (g_gcVM && c) g_gcVM->onBarrierObject(c->gc, false, c);
}
void gcBarrierValue(const Value& v) {
    if (g_gcVM) g_gcVM->onBarrierValue(v);
}
void gcBarrierUpvalue(Upvalue* up) {
    if (g_gcVM) g_gcVM->onBarrierUpvalue(up);
}

// 原生库 ABI 实现（宿主侧）：把函数表注入给库
static VM* asVM(AeVM* v) { return reinterpret_cast<VM*>(v); }

static void api_push_null(AeVM* v)                     { asVM(v)->push(Value()); }
static void api_push_int(AeVM* v, int64_t x)           { asVM(v)->push(Value(x)); }
static void api_push_float(AeVM* v, double x)          { asVM(v)->push(Value(x)); }
static void api_push_bool(AeVM* v, int b)              { asVM(v)->push(Value(b != 0)); }
static void api_push_str(AeVM* v, const char* s, size_t n) {
    asVM(v)->push(Value(aeAllocString(s ? string(s, n) : string())));
}
static void api_push_table(AeVM* v, AeValue t) {
    VM* vm = asVM(v);
    Table* table = vm->findNativeTable((const Table*)t.p);
    if (!table) { vm->pendingNativeError = "原生库 push 了非法的表引用"; return; }
    vm->push(Value::makeTable(table));
}
static AeValue api_make_str(AeVM* v, const char* s, size_t n) {
    VM* vm = asVM(v);
    AeValue a; a.kind = AE_NULL; a.i = 0; a.f = 0; a.p = nullptr;
    if (vm->nativeRoots.empty()) {
        vm->pendingNativeError = "make_str 只能在原生调用期间使用";
        return a;
    }
    GcString* sp = aeAllocString(s ? string(s, n) : string());
    vm->nativeRoots.back().strings.push_back(sp);
    a.kind = AE_STRING; a.p = sp;
    return a;
}
static AeValue api_new_table(AeVM* v) {
    VM* vm = asVM(v);
    AeValue a; a.kind = AE_NULL; a.i = 0; a.f = 0; a.p = nullptr;
    if (vm->nativeRoots.empty()) {
        vm->pendingNativeError = "new_table 只能在原生调用期间使用";
        return a;
    }
    Table* t = vm->allocTable();
    vm->nativeRoots.back().tables.push_back(t);
    a.kind = AE_TABLE; a.p = t;
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
// ABI v2：脚本命令行参数与脚本路径
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

// ABI v3：读表。脚本传进来的表就是 VM 里的 Table（argv 在调用期间一直活着），
// 所以直接把 p 当 Table* 用；取到的值借用返回，不做拷贝。
// ABI v4：库持有闭包句柄 + 从原生库调用 Ae 函数
static int api_func_retain(AeVM* v, AeValue fn) {
    VM* vm = asVM(v);
    if (fn.kind != AE_FUNC || !fn.p) return 0;
    const Closure* want = (const Closure*)fn.p;
    // AeValue 里的 p 是裸指针，要恢复成 shared_ptr 得从"还活着的地方"找回本体。
    // 值一定来自脚本，所以扫一遍操作数栈 / 各帧局部 / 全局就够了。
    auto match = [&](const Value& val) { return val.kind == VAL_FUNC && val.fn && val.fn == want; };
    // ① 各层原生调用的实参或回调返回值。
    for (auto roots = vm->nativeRoots.rbegin(); roots != vm->nativeRoots.rend(); ++roots) {
        for (const Value& val : roots->args)
            if (match(val)) { vm->funcHandles.push_back(val.fn); return (int)vm->funcHandles.size(); }
        for (Closure* val : roots->funcs)
            if (val && val == want) { vm->funcHandles.push_back(val); return (int)vm->funcHandles.size(); }
    }
    // ② 兜底：操作数栈 / 各帧局部 / 全局（值可能来自某处引用）
    for (const Value& val : vm->stack)     if (match(val)) { vm->funcHandles.push_back(val.fn); return (int)vm->funcHandles.size(); }
    for (const CallFrame& fr : vm->callStack)
        for (const Value& val : fr.locals) if (match(val)) { vm->funcHandles.push_back(val.fn); return (int)vm->funcHandles.size(); }
    for (const Value& val : vm->globals)   if (match(val)) { vm->funcHandles.push_back(val.fn); return (int)vm->funcHandles.size(); }
    return 0;
}
static void api_func_release(AeVM* v, int h) {
    VM* vm = asVM(v);
    if (h > 0 && (size_t)h <= vm->funcHandles.size()) vm->funcHandles[(size_t)h - 1] = nullptr;
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
    if (res.kind == VAL_STR || res.kind == VAL_TABLE || res.kind == VAL_FUNC) {
        if (vm->nativeRoots.empty()) {
            vm->nativeCallError = "func_call 返回堆对象时没有活动的原生调用";
            if (err) *err = vm->nativeCallError.c_str();
            return 0;
        }
        VM::NativeRoots& roots = vm->nativeRoots.back();
        if (res.kind == VAL_STR && res.str) roots.strings.push_back(res.str);
        else if (res.kind == VAL_TABLE && res.tab) roots.tables.push_back(res.tab);
        else if (res.kind == VAL_FUNC && res.fn) roots.funcs.push_back(res.fn);
    }
    if (out) *out = vm->toAeValue(res);
    return 1;
}

static int api_table_get(AeVM* v, AeValue t, AeValue k, AeValue* out) {
    if (out) { out->kind = AE_NULL; out->i = 0; out->f = 0; out->p = nullptr; }
    if (!out) return 0;
    try {
        VM* vm = asVM(v);
        Table* table = vm->findNativeTable((const Table*)t.p);
        if (!table) return 0;
        Value key = vm->fromAeValue(k);      // 键只可能是 int/bool/float/string
        Value val;
        if (!table->get(key, val)) return 0;
        if (!vm->nativeRoots.empty()) {
            VM::NativeRoots& roots = vm->nativeRoots.back();
            if (val.kind == VAL_STR && val.str) roots.strings.push_back(val.str);
            else if (val.kind == VAL_TABLE && val.tab) roots.tables.push_back(val.tab);
            else if (val.kind == VAL_FUNC && val.fn) roots.funcs.push_back(val.fn);
        }
        *out = vm->toAeValue(val);
        return 1;
    } catch (...) {
        return 0;
    }
}
static int api_table_len(AeVM* v, AeValue t) {
    Table* table = asVM(v)->findNativeTable((const Table*)t.p);
    return table ? (int)table->length() : 0;
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

// 命令行入口 —— 不是 main：ae.cpp 可以被 AOT 生成的 .cpp 直接 include，
// 那时由生成的代码提供自己的 main。真正的 main 在 ae_main.cpp（薄壳）。
int aeRunMain(int argc, char** argv) {
    aecon::install();    // 第一件事：把 cout/cerr/cin 接到控制台（GBK 的 cmd 里中文不乱码）
    string path;
    size_t maxDepth = 10000;
    vector<string> scriptArgs;

    for (int i = 1; i < argc; i++) {
        // Windows 上 argv 是本机 ANSI（中文系统上是 GBK），工具链内部一律 UTF-8：
        // 转一次，脚本拿到的 os.args()、以及诊断里显示的文件名都不会乱码
        string a = aepath::ansiToUtf8(argv[i]);
        // 脚本路径之后的参数一律原样传给脚本，不再当作 ae 自己的选项：
        // 否则脚本永远拿不到 -h / --help / --version（会被 VM 自己吃掉）。
        if (!path.empty()) { scriptArgs.push_back(a); continue; }
        if ((a == "--max-depth" || a == "-m") && i + 1 < argc) {
            try { maxDepth = (size_t)stoull(argv[++i]); }
            catch (...) { aediag::printBrief(AE_E_USAGE, "--max-depth 需要一个正整数"); return 3; }
            continue;
        }
        if (a == "-h" || a == "--help") {
            cout << "Ae 虚拟机 (ae) 2.1.0\n"
                    "用法: ae [--max-depth N] <file.aeo> [脚本参数...]\n"
                    "  --max-depth N, -m N   调用深度上限（默认 10000）\n"
                    "  -h, --help            显示帮助\n"
                    "  --version             显示版本\n"
                    "<file.aeo> 之后的参数原样传给脚本（os.args() / args 库可见）\n"
                    "退出码: 0 成功  2 运行错误  3 用法/IO 错误\n";
            return 0;
        }
        if (a == "--version") { cout << "ae 2.1.0 (AeBc 1.8)\n"; return 0; }
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
        return ex.code;                      // P1-6：exit(n) 直接带退出码结束
    } catch (const exception& e) {
        renderRunError(vm, e.what(), vm.thrownCode.empty() ? nullptr : vm.thrownCode.c_str());
        return 2;                            // 运行错误退出码 = 2
    }
    return 0;
}
// NaN-boxing：字符串分配（定义在文件末尾 —— 需要完整的 VM）
// Value 里只存裸指针，引用计数已移除，字符串改由 GC 统一回收。
GcString* aeAllocString(const std::string& s) {
    if (g_gcVM) return g_gcVM->allocString(s);
    GcString* p = new GcString();          // 没有 VM 时的退化路径：进程结束才释放
    *p = s;
    p->gc = GcHeader();
    return p;
}
GcString* aeAllocString(std::string&& s) {
    if (g_gcVM) return g_gcVM->allocString(std::move(s));
    GcString* p = new GcString(std::move(s));
    p->gc = GcHeader();
    return p;
}
