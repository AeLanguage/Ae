#pragma once
// Ae 抽象语法树（AST）
//
// 前端与两个后端之间唯一的公共契约：
//
// 源码 → Lexer → Parser(ae_parser.h) → AST ─┬→ ae_emitbc.h  → .aeo 字节码
// └→ ae_emitcpp.h → C++ 源码
//
// 设计取舍（三条，都影响 AOT 的实现难度）：
//
// ① AST 不带类型。
// 编译器里那个 ResType（RT_INT / RT_DOUBLE / …）是发射期推断的结果，
// 用来决定发 IADD 还是 ADD。它不该进 AST —— 否则 AOT 后端拿到的是一棵
// "已经按栈式 VM 做过决策"的树，反而不自由。
//
// ② 每个节点带 line / col。
// 运行期错误要指回源码位置；报错诊断也是。位置信息留在树上最省事。
//
// ③ 语法糖保留为独立节点，不在 AST 层展开。
// 推导、字符串插值、链式比较、切片都各自是一个节点。
// 若在 AST 层就降级成基础节点的组合，需要凭空造临时变量（gensym），
// 会污染用户可见的作用域 —— 放到发射期做，临时槽是 VM 内部的，安全。
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include "ae_common.h"

namespace aeast {

// 二元 / 一元运算符：直接复用词法器的 TK 值，省掉一张映射表
// （ae_parser.h 里 include ae_lexer.h，这里只存 int 避免依赖方向反转）
using Op = int;

struct Node {
    int line = 0, col = 0;
    virtual ~Node() = default;
};

struct Expr : Node {
    // 结束位置（最后一个属于本表达式的 token，字节列）。
    // 运行期诊断要按它落账 —— 与旧编译器"记录刚消费掉的 token"等价。
    int endLine = 0, endCol = 0;
};
struct Stmt : Node {};

using ExprP = std::unique_ptr<Expr>;
using StmtP = std::unique_ptr<Stmt>;

// 字面量
struct NumLit : Expr {
    double  d = 0;        // 浮点字面量
    int64_t i = 0;        // 整数字面量（int64，不是 double）
    bool    isFloat = false;
};
struct StrLit : Expr {
    std::string s;
};
struct BoolLit : Expr {
    bool v = false;
};
struct NullLit : Expr {};
struct Ident : Expr {
    std::string name;
};

// 运算
struct Unary : Expr {
    Op    op = 0;         // TK_MINUS / TK_NOT / TK_TILDE
    ExprP e;
};
struct Binary : Expr {
    Op    op = 0;
    ExprP l, r;
};
// 链式比较：a < b < c。语义 = (a<b) and (b<c)，且中间操作数只求值一次。
// 保留为独立节点 —— 展开成 and 会让 b 求值两遍，有副作用时结果就错了。
struct Chain : Expr {
    std::vector<Op>    ops;      // n-1 个比较运算符（可含 TK_IN / TK_NOT+TK_IN）
    std::vector<bool>  negates;  // 是否带 not（用于 not in）
    std::vector<ExprP> operands; // n 个操作数
};
struct Ternary : Expr {
    ExprP c, a, b;
};

// 调用 / 索引 / 字段
struct Call : Expr {
    ExprP             callee;
    std::vector<ExprP> args;
};
struct Index : Expr {
    ExprP obj, key;
};
// 切片：a[start:end:step]，三段都可省略（省略的指针为空）
struct Slice : Expr {
    ExprP obj;
    ExprP start, end, step;
};
struct Field : Expr {
    ExprP        obj;
    std::string  name;
};
// 限定名调用：io.readFile(...)。编译期要查库导出表，所以单独一个节点。
struct QualCall : Expr {
    std::string       modName, funcName;
    std::vector<ExprP> args;
};
// 点号取字段但紧跟调用：t.f(...)。Ae 里与 Call(Field) 等价，但保留形态便于 AOT。
struct MethodCall : Expr {
    ExprP             obj;
    std::string       name;
    std::vector<ExprP> args;
};

// 复合字面量
struct TableItem {
    ExprP key;            // 为空 = 数组段元素（{1,2,3}）；非空 = 命名字段（{a=1}）
    ExprP value;
};
struct TableLit : Expr {
    std::vector<TableItem> items;
};

struct Param {
    std::string name;
    bool        variadic = false;   // 名字后带 ...
    ExprP       def;                // 默认值（必须是字面量；无默认则为空）
};
// 闭包表达式：func(a, b) { ... }
// name 可非空：`local f = func fact(n) { ... }` —— 具名是为了自引用递归，
// 这个函数名只在自己体内可见（老编译器里也是这个语义）。
struct Lambda : Expr {
    std::string        name;
    std::vector<Param> params;
    std::vector<StmtP> body;
};

// 推导：[e for x in t if c] / {k:v for x in t}
struct ComprItem {
    bool        isNumeric = false;  // true = `for i = 1, 10, 2`
    std::string kName, vName;       // for-in 的键名/值名（vName 可空）
    std::string nName;              // 数值 for 的变量名
    ExprP       iter;               // 被迭代的表达式
    ExprP       start, stop, stepv; // 数值 for 的三段
    ExprP       cond;               // if 过滤（可空）
};
struct Comprehension : Expr {
    bool        isDict = false;
    ExprP       value;              // 数组推导的表达式 / 字典推导的值
    ExprP       key;                // 字典推导的键
    std::vector<ComprItem> items;   // 多重 for（笛卡尔积）
};

// 语句
struct Block : Stmt {
    std::vector<StmtP> stmts;
};

struct LocalDecl : Stmt {
    std::vector<std::string> names;
    std::vector<ExprP>       values;
};
struct Assign : Stmt {
    std::vector<ExprP> targets;     // Ident / Index / Field
    std::vector<ExprP> values;
};
struct CompoundAssign : Stmt {
    ExprP target;
    Op    op = 0;                   // TK_ADDEQ … TK_DIVEQ
    ExprP value;
};

struct IfStmt : Stmt {
    struct Branch { ExprP cond; std::vector<StmtP> body; };
    Branch                 primary;
    std::vector<Branch>    elifs;
    std::vector<StmtP>     elseBody;
    bool                   hasElse = false;
};
struct WhileStmt : Stmt {
    ExprP             cond;
    std::vector<StmtP> body;
};
struct DoWhileStmt : Stmt {
    std::vector<StmtP> body;
    ExprP              cond;
};
struct ForInStmt : Stmt {
    std::string        kName, vName;   // vName 为空 = `for k in t`
    ExprP              iter;
    std::vector<StmtP> body;
};
struct ForNumStmt : Stmt {
    std::string        name;
    ExprP              start, end, stepv;   // stepv 可空
    std::vector<StmtP> body;
};
struct ReturnStmt : Stmt {
    std::vector<ExprP> values;
};
struct BreakStmt : Stmt {
    int level = 1;   // break 2 表示作用于外层第 2 层循环
};
struct ContinueStmt : Stmt {
    int level = 1;
};
struct ExprStmt : Stmt {
    ExprP e;
};
struct DeferStmt : Stmt {
    ExprP call;                     // 必须是 Call / QualCall / MethodCall
};
struct TryStmt : Stmt {
    std::vector<StmtP> body;
    std::string        catchVar;
    std::vector<StmtP> catchBody;
    bool               hasCatch = false;
    std::vector<StmtP> finallyBody;
    bool               hasFinally = false;
};

// 函数定义（顶层与嵌套都是语句）
struct FuncDef : Stmt {
    std::string        name;
    std::vector<Param> params;
    std::vector<StmtP> body;
};

// 模块顶层变量（.m 里 `pi = 3.14` / `local x = 1`）
struct ModuleVar : Stmt {
    std::string name;
    ExprP       value;
    bool        exported = true;   // local = 不导出
};

struct ImpStmt : Stmt {
    std::string        modName;
    std::string        alias;      // 暂未启用
};

// 编译单元
struct Program {
    std::vector<StmtP> stmts;
    std::string        sourcePath;
    // 模块：一个 .ae 会把它 imp 的 .m 一起编进来，每个模块一棵 Program
    std::string        moduleName;     // 为空 = 主文件
    bool               isModule = false;
};

// 便捷构造（parser 里大量使用，省掉 make_unique 的噪音）
// 注意：Node 有虚函数，子类不是聚合类型，不能用 T{...} 聚合初始化。
// 所以只允许默认构造，字段由调用方逐一赋值。
template <class T>
std::unique_ptr<T> mk() {
    return std::unique_ptr<T>(new T());
}

} // namespace aeast
