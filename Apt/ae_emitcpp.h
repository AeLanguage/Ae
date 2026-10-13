// ae_emitcpp.h —— AOT 后端：AST → C++ 源码
// 为什么转 C++ 而不是 C
// ① try/catch → C++ 异常（P4）；C 只能 setjmp/longjmp 且局部变量要 volatile
// ② 影子栈用 RAII 自动注销（defer / 多出口 return 都不漏）
// ③ 运行时 ae.cpp 是 C++，直接 include，不用包一层 extern "C"
//
// 值模型
// 直接用运行时的 Value（16 字节 trivially copyable），不另造一套表示 ——
// 否则 AOT 与 VM / 原生库传值要转换，还会破坏 == 的身份语义。
//
// 影子栈（AOT 最大的坑）
// C++ 局部变量落在 C 栈上，GC 看不见。所以每个帧开一块 Value 数组 S[]，
// 局部变量与表达式临时值都放里面，进帧 shadowPush(S,N)，RAII 保证
// 任何返回路径（含异常）都注销。
//
// 函数签名
// static int ae_f_NAME(VM*, int argc, Value* argv, Value* rv)
// 返回"返回值个数"，值写进 rv[] —— Ae 支持多返回值，单返回值用 rv[0]。
//
// 覆盖范围
// 语言特性基本齐全。不支持协程（coroutine.*）与 run()：两者依赖切换
// 调用栈，C++ 栈无法 unwind，遇到在编译期报错。
#ifndef AE_EMITCPP_H
#define AE_EMITCPP_H

#include <string>
#include <vector>
#include <map>
#include <set>
#include <cstdio>
#include "ae_ast.h"

namespace aecpp {

inline const std::map<std::string,int>& cppBuiltinNames() {
    static const std::map<std::string,int> k = {

        {"pr",0},{"inp",1},{"prln",2},{"len",3},{"str",4},{"int",5},{"float",6},{"type",7},
        {"assert",8},{"error",9},{"exit",10},{"substr",11},{"ord",12},{"chr",13},{"find",14},
        {"upper",15},{"lower",16},{"trim",17},{"replace",18},{"split",19},{"join",20},
        {"repeat",21},{"format",22},{"delete",23},{"insert",24},{"pop",25},{"append",26},
        {"count",27},{"copy",28},{"deepcopy",29},{"equal",30},{"raise",31},{"run",32},
        {"weak",33},{"collect",34},{"gcstats",35},{"sort",36},{"sortBy",37},
    };
    return k;
}
inline int cppBuiltinId(const std::string& n) {
    auto it = cppBuiltinNames().find(n);
    return it == cppBuiltinNames().end() ? -1 : it->second;
}

inline std::string cppEsc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            case '\0': o += "\\0";  break;
            default:
                if (c < 0x20 || c == 0x7f) { char b[8]; snprintf(b,sizeof b,"\\x%02x",c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}
inline std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> v; std::string cur;
    for (char c : s) { if (c == '\n') { v.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) v.push_back(cur);
    return v;
}

inline std::string cppPreamble() {
    std::string s;
    s += "// 由 aec --aot 生成，请勿手工修改。\n";
    s += "#include \"ae.cpp\"\n\n";
    s += "// 多返回值缓冲区：帧内固定一块。调用后立刻取用 —— 取值时机总在\n"
         "//   下一次调用之前，所以共用一块不会被嵌套调用破坏。\n"
         "static const int AE_MAX_RET = 8;\n\n";

    // 基础工具（顺序有依赖：aeStr 最早）
    s += "static Value aeStr(VM* vm, const std::string& s) {\n"
         "    return Value(vm->allocString(std::string(s)));\n"
         "}\n";
    s += "static Value aeNeg(VM* vm, const Value& v) {\n"
         "    if (v.kind == VAL_DOUBLE) return vm->doSub(Value((double)0.0), v);\n"
         "    return vm->doSub(Value((int64_t)0), v);\n"
         "}\n";
    s += "static Value aeCmp(VM* vm, int op, const Value& l, const Value& r) {\n"
         "    vm->push(l); vm->push(r); vm->doCompare((uint8_t)op); return vm->pop();\n"
         "}\n";
    s += "static Value aeIndex(VM* vm, const Value& c, const Value& k) {\n"
         "    if (c.kind == VAL_STR) {\n"
         "        if (k.kind != VAL_INT) throw std::runtime_error(\"字符串索引必须是 int\");\n"
         "        int64_t n = (int64_t)utf8Length(c.str ? *c.str : std::string());\n"
         "        int64_t idx = k.i; if (idx < 0) idx = idx + n + 1;\n"
         "        std::string ch = utf8CharAt(c.str ? *c.str : std::string(), idx);\n"
         "        if (ch.empty() && idx > 0) return Value();\n"
         "        return Value(ch);\n"
         "    }\n"
         "    if (!c.isTable()) throw std::runtime_error(\"非表类型不能取下标\");\n"
         "    Value useKey = k;\n"
         "    if (k.kind == VAL_INT && k.i < 0) {\n"
         "        int64_t n = (int64_t)lenOfTable(*c.tab);\n"
         "        useKey = Value(k.i + n + 1);\n"
         "    }\n"
         "    Value o; if (!c.tab->get(useKey, o)) o = Value();\n"
         "    return o;\n"
         "}\n";
    s += "static void aeSetIndex(VM* vm, Value& c, const Value& k, const Value& v) {\n"
         "    (void)vm;\n"
         "    if (!c.isTable()) throw std::runtime_error(\"非表类型不能按 key 赋值\");\n"
         "    Value useKey = k;\n"
         "    if (k.kind == VAL_INT && k.i < 0) {\n"
         "        int64_t n = (int64_t)lenOfTable(*c.tab);\n"
         "        int64_t idx = k.i + n + 1;\n"
         "        if (idx < 1) throw std::runtime_error(\"表索引越界（负索引 \" + std::to_string(k.i)\n"
         "            + \" 折算后是 \" + std::to_string(idx) + \"，1-based 最小为 1）\");\n"
         "        useKey = Value(idx);\n"
         "    }\n"
         "    c.tab->set(useKey, v);\n"
         "}\n";
    s += "// AOT 帧链：C++ 栈不是 Ae 栈，靠它打印 \"在 xxx()\"\n"
         "struct AeFrameLink {\n"
         "    VM* vm;\n"
         "    AeFrameLink(VM* v, const char* fn, const char* file, int line) : vm(v) {\n"
         "        vm->aotFrames.push_back({fn, file, line});\n"
         "    }\n"
         "    ~AeFrameLink() { if (!vm->aotFrames.empty()) vm->aotFrames.pop_back(); }\n"
         "};\n"
         "static std::string aeFramesText(VM* vm) {\n"
         "    std::string o;\n"
         "    for (size_t i = vm->aotFrames.size(); i-- > 0; )\n"
         "        o += \"\\n  在 \" + std::string(vm->aotFrames[i].fn) + \"() \" +\n"
         "              vm->aotFrames[i].file + \":\" + std::to_string(vm->aotFrames[i].line);\n"
         "    return o;\n"
         "}\n\n";
    s += "// Ae 错误：带抛出位置（由 aeCallBuiltin 等按调用点填入）\n"
         "struct AeErrPos { std::string msg, file, stack; int line, col;\n"
         "    AeErrPos(std::string m, std::string f, int l, int c, std::string st)\n"
         "        : msg(std::move(m)), file(std::move(f)), stack(std::move(st)), line(l), col(c) {} };\n"
         // 与 VM 的 buildErrorTable 对齐：{code, kind, msg, file, line, col, stack}
         "static Value aeErrTable(VM* vm, const std::string& msg, const std::string& file, int line, int col,\n"
         "                         const std::string& stack) {\n"
         "    Table* t = vm->allocTable();\n"
         "    const char* code = vm->thrownCode.empty() ? classifyRun(msg) : vm->thrownCode.c_str();\n"
         "    t->set(aeStr(vm, \"code\"), Value(std::string(code)));\n"
         "    t->set(aeStr(vm, \"kind\"), Value(std::string(kindForCode(code))));\n"
         "    t->set(aeStr(vm, \"msg\"),  Value(msg));\n"
         "    t->set(aeStr(vm, \"file\"), Value(file));\n"
         "    t->set(aeStr(vm, \"line\"), Value((int64_t)line));\n"
         "    t->set(aeStr(vm, \"col\"),  Value((int64_t)col));\n"
         "    t->set(aeStr(vm, \"stack\"), Value(stack));\n"
         "    return Value::makeTable(t);\n"
         "}\n\n";
    s += "static Value aeCallBuiltin(VM* vm, int id, Value* args, int n,\n"
         "                           const char* file, int line, int col) {\n"
         "    for (int i = 0; i < n; i++) vm->push(args[i]);\n"
         "    if (id == 9 || id == 31) vm->thrownCode = AE_E_USER_ERROR;\n"
         "    try { vm->invoke((uint8_t)id, (uint8_t)n, 1); }\n"
         "    catch (const ExitException&) { throw; }\n"
         "    catch (const std::exception& e) {\n"
         "        const AeErrPos* ip = dynamic_cast<const AeErrPos*>(&e);\n"
         "        if (ip) throw AeErrPos(ip->msg, ip->file, ip->line, ip->col, ip->stack);\n"   // 最内层位置优先
         "        throw AeErrPos(e.what(), file, line, col, aeFramesText(vm));\n"
         "    }\n"
         "    return vm->pop();\n"
         "}\n";
    s += "static Value aeKeys(VM* vm, const Value& v) {\n"
         "    if (!v.isTable()) throw std::runtime_error(\"for-in 需要一个表\");\n"
         "    Table* out = vm->allocTable();\n"
         "    const Table* t = v.tab;\n"
         "    for (size_t i = 0; i < t->arr.size(); i++)\n"
         "        if (t->arr[i].kind != VAL_NULL) out->arr.push_back(Value((int64_t)(i+1)));\n"
         "    t->mapv.forEachSorted([&](const HashMap::Slot& hs) {\n"
         "        if (hs.val.kind == VAL_NULL) return;\n"
         "        switch (hs.key.kind) {\n"
         "            case VAL_INT:    out->arr.push_back(Value((int64_t)hs.key.i)); break;\n"
         "            case VAL_BOOL:   out->arr.push_back(Value(hs.key.i != 0)); break;\n"
         "            case VAL_DOUBLE: out->arr.push_back(Value(hs.key.f)); break;\n"
         "            case VAL_STR:    out->arr.push_back(Value(hs.key.str ? *hs.key.str : std::string())); break;\n"
         "            default: break;\n"
         "        }\n"
         "    });\n"
         "    return Value::makeTable(out);\n"
         "}\n";
    s += "static Value aeSlice(VM* vm, const Value& c, const Value& a, const Value& b, const Value& st) {\n"
         "    return vm->doSlice(c, a, b, st);\n"
         "}\n";
    s += "static Value aeIn(VM* vm, const Value& n, const Value& c) { return vm->doIn(n, c); }\n";
    s += "static bool aeStepNeg(const Value& st) {\n"
         "    if (st.kind == VAL_DOUBLE) return st.f < 0;\n"
         "    if (st.kind == VAL_INT) return st.i < 0;\n"
         "    return false;\n"
         "}\n\n";

    // 错误 / defer / 影子栈 / 帧链
    s += "static int aeBuiltinId(const std::string& n);\n";
    s += "static Value aePow(VM* vm, const Value& l, const Value& r) {\n"
         "    if (l.kind != VAL_INT && l.kind != VAL_DOUBLE) throw std::runtime_error(\"** 要求两侧都是数字\");\n"
         "    if (r.kind != VAL_INT && r.kind != VAL_DOUBLE) throw std::runtime_error(\"** 要求两侧都是数字\");\n"
         "    if (l.kind == VAL_INT && r.kind == VAL_INT && r.i >= 0 && r.i <= 63) {\n"
         "        int64_t res = 1, base = l.i;\n"
         "        for (int64_t k = 0; k < r.i; k++) res = vm->wrapMul(res, base);\n"
         "        return Value(res);\n"
         "    }\n"
         "    return Value(pow(vm->toDouble(l), vm->toDouble(r)));\n"
         "}\n";
    s += "static Value aeBin(VM* vm, const char* op, const Value& a, const Value& b,\n"
         "                   const char* file, int line, int col) {\n"
         "    try {\n"
         "        if (!strcmp(op,\"doAdd\")) return vm->doAdd(a, b);\n"
         "        if (!strcmp(op,\"doSub\")) return vm->doSub(a, b);\n"
         "        if (!strcmp(op,\"doMul\")) return vm->doMul(a, b);\n"
         "        if (!strcmp(op,\"doDiv\")) return vm->doDiv(a, b);\n"
         "        if (!strcmp(op,\"doMod\")) return vm->doMod(a, b);\n"
         "        return Value();\n"
         "    } catch (const AeErrPos&) { throw; }\n"
         "    catch (const std::exception& e) {\n"
         "        throw AeErrPos(e.what(), file, line, col, aeFramesText(vm)); }\n"
         "}\n";
    s += "static Value aeBitNot(const Value& a) {\n"
         "    if (a.kind != VAL_INT) throw std::runtime_error(std::string(\"位运算符 ~ 只支持 int，得到 \")\n"
         "        + typeName(a.kind) + (a.kind == VAL_DOUBLE ? \"（float 请用 int(x) 显式转换）\" : \"\"));\n"
         "    return Value((int64_t)~a.i);\n"
         "}\n";
    s += "static Value aeBit(VM* vm, const char* sym, int op, const Value& a, const Value& b) {\n"
         "    (void)vm;\n"
         "    if (a.kind != VAL_INT || b.kind != VAL_INT) {\n"
         "        bool isF = (a.kind == VAL_DOUBLE || b.kind == VAL_DOUBLE);\n"
         "        const Value& bad = (a.kind != VAL_INT) ? a : b;\n"
         "        throw std::runtime_error(std::string(\"位运算符 \") + sym + \" 只支持 int，得到 \"\n"
         "            + typeName(bad.kind) + (isF ? \"（float 请用 int(x) 显式转换）\" : \"\"));\n"
         "    }\n"
         "    int64_t x = a.i, y = b.i, r = 0;\n"
         "    switch (op) {\n"
         "      case 0: r = x & y;  break;\n"
         "      case 1: r = x | y;  break;\n"
         "      case 2: r = x ^ y;  break;\n"
         "      case 3: r = (y >= 0 && y < 64) ? (int64_t)((uint64_t)x << y) : 0; break;\n"
         "      case 4: r = (y >= 0 && y < 64) ? (x >> y) : (x < 0 ? (int64_t)-1 : (int64_t)0); break;\n"
         "    }\n"
         "    return Value(r);\n"
         "}\n";
    s += "static Value aeCallNative(VM* vm, const char* key, Value* args, int n) {\n"
         "    return vm->callNativeValue(key, args, n);\n"
         "}\n";
    s += "// finally 里抛的错要盖掉正在传播的异常（VM 语义）。\n"
         "//   但 C++ 在栈展开期从析构器抛出会 terminate，所以展开期只能记下来，\n"
         "//   等 try 语句整体结束后再重抛。\n"
         "static std::vector<AeErrPos>& aePending() { static std::vector<AeErrPos> v; return v; }\n\n";
    s += "static void aeDoDefer(VM* vm, const Value& dv) {\n"
         "    if (!dv.isTable() || !dv.tab) return;\n"
         "    Value fn, ap;\n"
         "    dv.tab->get(Value((int64_t)1), fn);\n"
         "    std::vector<Value> av;\n"
         "    if (dv.tab->get(aeStr(vm, \"args\"), ap) && ap.isTable())\n"
         "        for (size_t i = 0; i < ap.tab->arr.size(); i++) av.push_back(ap.tab->arr[i]);\n"
         "    if (fn.isFunc() && fn.fn && fn.fn->aotFn) {\n"
         "        Value rv[AE_MAX_RET];\n"
         "        fn.fn->aotFn(vm, (int)av.size(), av.empty() ? nullptr : av.data(), rv,\n"
         "                     fn.fn->ups.empty() ? nullptr : fn.fn->ups.data());\n"
         "        return;\n"
         "    }\n"
         "    { Value nm; if (dv.tab->get(aeStr(vm, \"name\"), nm) && nm.kind == VAL_STR) {\n"
         "        int bid = aeBuiltinId(*nm.str);\n"
         "        if (bid >= 0) aeCallBuiltin(vm, bid, av.empty() ? nullptr : av.data(), (int)av.size(),\n"
         "                                   \"?\", 0, 0);\n"
         "    } }\n"
         "}\n";
    s += "static Value aeMakeDefer(VM* vm, const Value& fn, const std::string& name, const std::vector<Value>& args) {\n"
         "    Table* d = vm->allocTable();\n"
         "    d->set(Value((int64_t)1), fn);\n"
         "    Table* at = vm->allocTable();\n"
         "    for (size_t i = 0; i < args.size(); i++) at->set(Value((int64_t)(i+1)), args[i]);\n"
         "    d->set(aeStr(vm, \"args\"), Value::makeTable(at));\n"
         "    d->set(aeStr(vm, \"name\"), Value(name));\n"
         "    return Value::makeTable(d);\n"
         "}\n";
    s += "// defer 作用域：析构时反序执行本作用域登记的 defer\n"
         "struct AeDeferScope {\n"
         "    VM* vm; std::vector<Value> ds;\n"
         "    AeDeferScope(VM* v) : vm(v) {}\n"
         "    ~AeDeferScope() noexcept {\n"
         "        for (size_t i = ds.size(); i-- > 0; ) {\n"
         "            try { aeDoDefer(vm, ds[i]); } catch (...) {}\n"
         "        }\n"
         "    }\n"
         "};\n"
         "static std::vector<AeDeferScope*> _ds_all;\n"
         "static AeDeferScope* _ds_top() { return _ds_all.empty() ? nullptr : _ds_all.back(); }\n"
         "struct AeDsReg { AeDsReg(AeDeferScope* s) { _ds_all.push_back(s); }\n"
         "                 ~AeDsReg() { _ds_all.pop_back(); } };\n\n";
    s += "struct AeShadowFrame {\n"
         "    VM* vm; bool hasBoxes;\n"
         "    AeShadowFrame(VM* v, Value* s, size_t n) : vm(v), hasBoxes(false) { vm->shadowPush(s, n); }\n"
         "    AeShadowFrame(VM* v, Value* s, size_t n, Upvalue** b, size_t nb) : vm(v), hasBoxes(true) {\n"
         "        vm->shadowPush(s, n); vm->shadowPushBoxes(b, nb);\n"
         "    }\n"
         "    ~AeShadowFrame() { if (hasBoxes) vm->shadowPopBoxes(); vm->shadowPop(); }\n"
         "};\n\n";
    s += "static Value aeCallValue(VM* vm, const Value& f, int argc, Value* argv) {\n"
         "    if (!f.isFunc() || !f.fn) throw std::runtime_error(\"试图调用一个不是函数的值\");\n"
         "    if (!f.fn->aotFn) throw std::runtime_error(\"AOT: 只能调用 AOT 生成的函数\");\n"
         "    Value rv[AE_MAX_RET];\n"
         "    for (int i = 0; i < AE_MAX_RET; i++) rv[i] = Value();\n"
         "    int n = f.fn->aotFn(vm, argc, argv, rv,\n"
         "                        f.fn->ups.empty() ? nullptr : f.fn->ups.data());\n"
         "    return n > 0 ? rv[0] : Value();\n"
         "}\n\n";

    // main 兜底：把 AeErr 转成带帧链的运行错误
    return s;
}

// .m 模块：编译期内联进生成的 C++
// （与字节码后端同一套语义）
using namespace aeast;
struct ModInfo {
    std::set<std::string>              funcs;
    std::map<std::string,int>          varIdx;
    std::shared_ptr<aeast::Program>    prog;
};
inline std::map<std::string, ModInfo>& modTable()    { static std::map<std::string, ModInfo> t; return t; }
inline std::vector<std::string>&       modLoadOrder(){ static std::vector<std::string> v; return v; }
inline std::set<std::string>&          nativeLibSet() { static std::set<std::string> s; return s; }

inline bool findModulePath(const std::string& name, const std::string& srcName, std::string& out) {
    std::vector<std::string> dirs;
    if (const char* env = getenv("AE_LIB_PATH")) if (*env) dirs.push_back(env);
    std::string ed = aehost::exeDir();
    if (!ed.empty()) { dirs.push_back(ed + "/lib"); dirs.push_back(ed); }
    std::string sd = aehost::dirOf(srcName);
    if (!sd.empty() && sd != ed) { dirs.push_back(sd + "/lib"); dirs.push_back(sd); }
    dirs.push_back("lib"); dirs.push_back(".");
    for (const std::string& d : dirs) {
        std::string real;
        if (aepath::resolvePath(d + "/" + name + ".m", &real)) { out = real; return true; }
    }
    return false;
}

inline void collectModule(const std::string& name, const std::string& srcName,
                          std::set<std::string>& stack) {
    if (modTable().count(name)) return;
    for (const std::string& m : stack)
        if (m == name) throw std::runtime_error("循环导入：'" + name + "' 已经在导入链上了");
    std::string path;
    if (!findModulePath(name, srcName, path)) return;      // 不是 .m → 原生库
    stack.insert(name);
    std::string src;
    if (!aepath::readFileText(path, &src)) throw std::runtime_error("无法打开模块文件: " + path);
    aeparse::Parser P(src, path, 0);
    auto prog = std::make_shared<aeast::Program>(P.parseProgram(name));
    for (auto& st : prog->stmts) {                          // 先递归处理模块自己的 imp
        auto* im = dynamic_cast<ImpStmt*>(st.get());
        if (!im) continue;
        std::string mp;
        if (findModulePath(im->modName, path, mp)) collectModule(im->modName, path, stack);
        else nativeLibSet().insert(im->modName);             // 原生库（含模块内部 imp 的）
    }
    ModInfo mi; mi.prog = prog;
    for (auto& st : prog->stmts)
        if (auto* f = dynamic_cast<FuncDef*>(st.get())) mi.funcs.insert(f->name);
    for (auto& st : prog->stmts) {
        if (auto* l = dynamic_cast<LocalDecl*>(st.get())) {
            for (auto& n : l->names) if (!mi.varIdx.count(n)) mi.varIdx[n] = (int)mi.varIdx.size();
        }
        else if (auto* a = dynamic_cast<Assign*>(st.get())) {
            for (auto& t : a->targets) {
                if (auto* id = dynamic_cast<Ident*>(t.get()))
                    if (!mi.varIdx.count(id->name)) mi.varIdx[id->name] = (int)mi.varIdx.size();
            }
        }
    }
    modTable()[name] = std::move(mi);
    modLoadOrder().push_back(name);
    stack.erase(name);
}

class CppGen {
public:
    std::string out;
    std::string ind = "";
    std::vector<std::map<std::string,int>> scopes;
    int slotTop = 0, tmpBase = 0, tmpTop = 0, maxSlot = 0;
    std::map<std::string,int> globals;
    std::set<std::string> funcNames;
    int lblCnt = 0;
    // 循环上下文：break/continue 的落点
    std::vector<std::string> brkLbl, contLbl;
    // 上值 / 盒子（P3 闭包）
    std::vector<std::string>        upNames;      // 本函数继承的上值名（顺序 = ups 数组顺序）
    std::map<std::string,int>       upIdx;
    std::set<std::string>           boxedNames;   // 本函数中"被嵌套闭包捕获"的局部/参数名
    std::map<int,int>               boxOfSlot;    // slot → BX 下标
    int                             nBox = 0;
    std::map<std::string,int>       boxByName;
    bool                            inFunction = false;
    std::string                     fnName;       // 当前函数名（报错用）
    std::string                     srcFile;      // 源文件名（错误表用）
    std::set<std::string>           nativeMods;
    std::map<std::string,std::string> nativeFuncs;  // 无限定名 → "mod.func" // imp 进来的原生库名（io / os / path …）
    std::string                     curMod;       // 当前所在 .m 模块（空 = 主程序）
    std::string                     modInitOf;    // 正在生成某模块的顶层变量 init
    int                             fnLine = 0;   // 函数定义行（栈回溯用）
    std::string nestedCode;                       // 嵌套 lambda 生成的 C++ 函数体（汇总后输出）
    std::vector<int> deferDepth;                  // 每个作用域层登记的 defer 数量
    std::vector<std::string> deferStores;         // 每层 defer 存放的槽名

    void line(const std::string& s) { out += ind + s + "\n"; }
    void push() { scopes.push_back({}); }
    void pop()  { scopes.pop_back(); }
    // 块作用域：进入时开一层 defer 收集，退出时生成"反序执行"的 RAII 析构
    void pushBlock() { push(); deferDepth.push_back(0); deferStores.push_back(""); }
    void popBlock()  { pop();  if (!deferDepth.empty()) deferDepth.pop_back();
                               if (!deferStores.empty()) deferStores.pop_back(); }
    int  declare(const std::string& n) {
        { auto f = scopes.back().find(n);
          if (f != scopes.back().end()) return f->second; }   // 同层同名复用（自引用闭包要）
        int s = slotTop++;
        scopes.back()[n] = s;
        if (boxedNames.count(n) && !boxOfSlot.count(s)) { boxOfSlot[s] = nBox; boxByName[n] = nBox; nBox++; }
        if (slotTop > maxSlot) maxSlot = slotTop;
        // 生成期新声明的局部（如 catch 变量）必须把临时区起点顶开，
        // 否则后续 allocTmp 会复用它的槽，把值覆盖掉。
        if (slotTop > tmpBase) tmpBase = slotTop;
        return s;
    }
    // 变量引用：被捕获的走堆上的盒子（BX[k]->get() 是 Value&，可直接当左值）
    int lastSlot = -1, lastUp = -1;
    std::string ref(int slot) const {
        if (slot <= -1000) return "UP[" + std::to_string(-1000 - slot) + "]->get()";
        auto it = boxOfSlot.find(slot);
        if (it != boxOfSlot.end()) return "BX[" + std::to_string(it->second) + "]->get()";
        return "S[" + std::to_string(slot) + "]";
    }
    std::set<std::string> allLocals() const {
        std::set<std::string> r;
        for (auto& sc : scopes) for (auto& kv : sc) r.insert(kv.first);
        return r;
    }
    bool lookup(const std::string& n, int& slot) const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto f = it->find(n);
            if (f != it->end()) { slot = f->second; return true; }
        }
        return false;
    }
    int allocTmp() { if (tmpTop < tmpBase) tmpTop = tmpBase;   // declare 顶开下界后要同步
        int t = tmpTop++; if (t + 1 > maxSlot) maxSlot = t + 1; return t; }
    void resetTmp() { tmpTop = tmpBase; }
    std::string S(int i) const { return "S[" + std::to_string(i) + "]"; }
    std::string newLbl(const char* p) { return std::string(p) + std::to_string(++lblCnt); }
    void dedent(int n = 4) { if ((int)ind.size() >= n) ind.resize(ind.size() - n); }
    // 只有局部变量（含参数）才用 declare；全局名走 globals
    bool hasLocal(const std::string& n) const { int s; return lookup(n, s); }
    int  globalSlot(const std::string& n) const {
        auto it = globals.find(n);
        return it == globals.end() ? -1 : it->second;
    }
};

// 前向声明
inline void genStmt(CppGen& g, aeast::Stmt* s);
inline void genCatchClauses(CppGen& g, aeast::TryStmt* t, bool innerNested);
inline void genStmts(CppGen& g, const std::vector<aeast::StmtP>& v);
inline int  genExpr(CppGen& g, aeast::Expr* e);
inline int  genLambda(CppGen& g, aeast::Lambda* L);
inline void prescan(CppGen& g, aeast::Stmt* s);

// 自由变量分析（P3 闭包）
inline void collectIdents(aeast::Expr* e, std::vector<std::string>& out);
inline void collectIdents(aeast::Stmt* s, std::vector<std::string>& out) {
    using namespace aeast;
    if (!s) return;
    auto all = [&](const std::vector<StmtP>& v) { for (auto& x : v) collectIdents(x.get(), out); };
    if (auto* b = dynamic_cast<Block*>(s)) { all(b->stmts); return; }
    if (auto* l = dynamic_cast<LocalDecl*>(s)) {
        for (auto& e : l->values) collectIdents(e.get(), out);
        return;
    }
    if (auto* a = dynamic_cast<Assign*>(s)) {
        for (auto& t : a->targets) collectIdents(t.get(), out);
        for (auto& e : a->values) collectIdents(e.get(), out);
        return;
    }
    if (auto* c = dynamic_cast<CompoundAssign*>(s)) {
        collectIdents(c->target.get(), out);
        collectIdents(c->value.get(), out);
        return;
    }
    if (auto* i = dynamic_cast<IfStmt*>(s)) {
        collectIdents(i->primary.cond.get(), out);
        all(i->primary.body);
        for (auto& b : i->elifs) {
            collectIdents(b.cond.get(), out);
            all(b.body);
        }
        if (i->hasElse) all(i->elseBody);
        return;
    }
    if (auto* w = dynamic_cast<WhileStmt*>(s)) {
        collectIdents(w->cond.get(), out);
        all(w->body);
        return;
    }
    if (auto* d = dynamic_cast<DoWhileStmt*>(s)) {
        all(d->body);
        collectIdents(d->cond.get(), out);
        return;
    }
    if (auto* f = dynamic_cast<ForNumStmt*>(s)) {
        collectIdents(f->start.get(), out);
        collectIdents(f->end.get(), out);
        collectIdents(f->stepv.get(), out);
        all(f->body);
        return;
    }
    if (auto* f = dynamic_cast<ForInStmt*>(s)) {
        collectIdents(f->iter.get(), out);
        all(f->body);
        return;
    }
    if (auto* r = dynamic_cast<ReturnStmt*>(s)) {
        for (auto& e : r->values) collectIdents(e.get(), out);
        return;
    }
    if (auto* e = dynamic_cast<ExprStmt*>(s)) { collectIdents(e->e.get(), out); return; }
    if (auto* d = dynamic_cast<DeferStmt*>(s)) { collectIdents(d->call.get(), out); return; }
    if (auto* t = dynamic_cast<TryStmt*>(s)) {
        all(t->body);
        for (auto& x : t->catchBody) collectIdents(x.get(), out);
        for (auto& x : t->finallyBody) collectIdents(x.get(), out);
        return;
    }
    if (auto* f = dynamic_cast<FuncDef*>(s)) {
        for (auto& st : f->body) collectIdents(st.get(), out);
        return;
    }
}
inline void collectIdents(aeast::Expr* e, std::vector<std::string>& out) {
    using namespace aeast;
    if (!e) return;
    if (auto* id = dynamic_cast<Ident*>(e))  { out.push_back(id->name); return; }
    if (auto* u = dynamic_cast<Unary*>(e))   { collectIdents(u->e.get(), out); return; }
    if (auto* b = dynamic_cast<Binary*>(e))  { collectIdents(b->l.get(), out); collectIdents(b->r.get(), out); return; }
    if (auto* c = dynamic_cast<Chain*>(e))   { for (auto& x : c->operands) collectIdents(x.get(), out); return; }
    if (auto* t = dynamic_cast<Ternary*>(e)) { collectIdents(t->c.get(), out); collectIdents(t->a.get(), out);
                                               collectIdents(t->b.get(), out); return; }
    if (auto* c = dynamic_cast<Call*>(e)) {
        collectIdents(c->callee.get(), out);
        for (auto& a : c->args) collectIdents(a.get(), out);
        return;
    }
    if (auto* i = dynamic_cast<Index*>(e)) {
        collectIdents(i->obj.get(), out);
        collectIdents(i->key.get(), out);
        return;
    }
    if (auto* s2 = dynamic_cast<Slice*>(e)) {
        collectIdents(s2->obj.get(), out);
        collectIdents(s2->start.get(), out);
        collectIdents(s2->end.get(), out);
        collectIdents(s2->step.get(), out);
        return;
    }
    if (auto* f = dynamic_cast<Field*>(e)) { collectIdents(f->obj.get(), out); return; }
    if (auto* q = dynamic_cast<QualCall*>(e)) {
        for (auto& a : q->args) collectIdents(a.get(), out);
        return;
    }
    if (auto* m = dynamic_cast<MethodCall*>(e)) {
        collectIdents(m->obj.get(), out);
        for (auto& a : m->args) collectIdents(a.get(), out);
        return;
    }
    if (auto* t = dynamic_cast<TableLit*>(e)) {
        for (auto& it : t->items) {
            collectIdents(it.key.get(), out);
            collectIdents(it.value.get(), out);
        }
        return;
    }
    if (auto* l = dynamic_cast<Lambda*>(e))  { for (auto& st : l->body) collectIdents(st.get(), out); return; }
    if (auto* c = dynamic_cast<Comprehension*>(e)) {
        collectIdents(c->value.get(), out); collectIdents(c->key.get(), out); return;
    }
}
inline void collectDeclared(aeast::Stmt* s, std::set<std::string>& out);
inline void collectDeclaredE(aeast::Expr* e, std::set<std::string>& out) {
    using namespace aeast;
    if (!e) return;
    if (auto* l = dynamic_cast<Lambda*>(e)) {
        for (auto& p : l->params) out.insert(p.name);
        for (auto& st : l->body) collectDeclared(st.get(), out);
    }
}
inline void collectDeclared(aeast::Stmt* s, std::set<std::string>& out) {
    using namespace aeast;
    if (!s) return;
    if (auto* b = dynamic_cast<Block*>(s)) {
        for (auto& x : b->stmts) collectDeclared(x.get(), out);
        return;
    }
    if (auto* l = dynamic_cast<LocalDecl*>(s)) {
        for (auto& n : l->names) out.insert(n);
        for (auto& e : l->values) collectDeclaredE(e.get(), out);
        return;
    }
    if (auto* f = dynamic_cast<FuncDef*>(s)) {
        out.insert(f->name);
        for (auto& p : f->params) out.insert(p.name);
        for (auto& st : f->body) collectDeclared(st.get(), out);
        return;
    }
    if (auto* i = dynamic_cast<IfStmt*>(s)) {
        for (auto& x : i->primary.body) collectDeclared(x.get(), out);
        for (auto& b : i->elifs)
            for (auto& x : b.body) collectDeclared(x.get(), out);
        for (auto& x : i->elseBody) collectDeclared(x.get(), out);
        return;
    }
    if (auto* w = dynamic_cast<WhileStmt*>(s)) {
        for (auto& x : w->body) collectDeclared(x.get(), out);
        return;
    }
    if (auto* d = dynamic_cast<DoWhileStmt*>(s)) {
        for (auto& x : d->body) collectDeclared(x.get(), out);
        return;
    }
    if (auto* f = dynamic_cast<ForNumStmt*>(s)) {
        out.insert(f->name);
        for (auto& x : f->body) collectDeclared(x.get(), out);
        return;
    }
    if (auto* f = dynamic_cast<ForInStmt*>(s)) {
        out.insert(f->kName);
        if (!f->vName.empty()) out.insert(f->vName);
        for (auto& x : f->body) collectDeclared(x.get(), out);
        return;
    }
    if (auto* e = dynamic_cast<ExprStmt*>(s)) { collectDeclaredE(e->e.get(), out); return; }
    if (auto* a = dynamic_cast<Assign*>(s)) {
        for (auto& e : a->values) collectDeclaredE(e.get(), out);
        return;
    }
    if (auto* r = dynamic_cast<ReturnStmt*>(s)) {
        for (auto& e : r->values) collectDeclaredE(e.get(), out);
        return;
    }
    if (auto* t = dynamic_cast<TryStmt*>(s)) {
        for (auto& x : t->body) collectDeclared(x.get(), out);
        if (t->hasCatch) out.insert(t->catchVar);
        for (auto& x : t->catchBody) collectDeclared(x.get(), out);
        for (auto& x : t->finallyBody) collectDeclared(x.get(), out);
        return;
    }
    if (auto* d = dynamic_cast<DeferStmt*>(s)) { collectDeclaredE(d->call.get(), out); return; }
}
// Lambda / 嵌套 FuncDef 的候选自由变量（按首次出现排序）
inline std::vector<std::string> freeVarsOf(const std::vector<aeast::StmtP>& body,
                                           const std::vector<aeast::Param>& params,
                                           const CppGen& ctx,
                                           const std::set<std::string>* locals = nullptr) {
    std::vector<std::string> seen;
    std::set<std::string> decl;
    for (auto& p : params) decl.insert(p.name);
    for (auto& st : body) collectDeclared(st.get(), decl);
    std::set<std::string> got;
    for (auto& st : body) {
        std::vector<std::string> ids; collectIdents(st.get(), ids);
        for (auto& n : ids) {
            if (decl.count(n) || got.count(n)) continue;
            if (cppBuiltinId(n) >= 0) continue;          // 内置
            if (ctx.funcNames.count(n)) continue;        // 全局函数 → 静态调用
            // locals 里出现过的名字是本层局部变量（哪怕同名也登记进了 globals），
            // 必须当自由变量捕获，否则闭包读到的是 G[] 的空槽
            if (!(locals && locals->count(n)) && ctx.globals.count(n)) continue;
            got.insert(n); seen.push_back(n);
        }
    }
    return seen;
}
// 本函数中"需要装箱"的局部名：任何直接嵌套的闭包捕获到的本地名
inline void computeBoxed(CppGen& g, const std::vector<aeast::StmtP>& body,
                         const std::vector<aeast::Param>& params = {}) {
    using namespace aeast;
    std::set<std::string> mine;
    for (auto& p : params) mine.insert(p.name);      // 参数也能被捕获
    for (auto& st : body) collectDeclared(st.get(), mine);
    // 直接嵌套的 Lambda / FuncDef（不穿过另一层 lambda，那层的自由变量由它自己的 upNames 传递）
    std::vector<std::pair<std::vector<StmtP>*, std::vector<Param>*>> nests;
    std::set<std::string> extra;
    std::function<void(Expr*)> walkE = [&](Expr* e) {
        if (!e) return;
        if (auto* l = dynamic_cast<Lambda*>(e)) { nests.push_back({&l->body, &l->params}); return; }
        if (auto* cp = dynamic_cast<Comprehension*>(e)) {   // 推导体里的 lambda
            for (auto& itm : cp->items) {
                if (itm.isNumeric) extra.insert(itm.nName);
                else { extra.insert(itm.kName); if (!itm.vName.empty()) extra.insert(itm.vName); }
            }
            walkE(cp->value.get()); walkE(cp->key.get());
            for (auto& itm : cp->items) { walkE(itm.iter.get()); walkE(itm.cond.get()); }
            return;
        }
        if (auto* u = dynamic_cast<Unary*>(e))   { walkE(u->e.get()); return; }
        if (auto* b = dynamic_cast<Binary*>(e))  { walkE(b->l.get()); walkE(b->r.get()); return; }
        if (auto* c = dynamic_cast<Call*>(e))    { for (auto& a : c->args) walkE(a.get()); return; }
        if (auto* t = dynamic_cast<TableLit*>(e)){ for (auto& it : t->items) walkE(it.value.get()); return; }
    };
    std::function<void(Stmt*)> walk = [&](Stmt* st) {
        if (!st) return;
        if (dynamic_cast<Lambda*>(st)) return;   // Lambda 是表达式，不递归
        if (auto* f = dynamic_cast<FuncDef*>(st)) { nests.push_back({&f->body, &f->params}); return; }
        if (auto* b = dynamic_cast<Block*>(st)) {
            for (auto& x : b->stmts) walk(x.get());
            return;
        }
        if (auto* i = dynamic_cast<IfStmt*>(st)) {
            for (auto& x : i->primary.body) walk(x.get());
            for (auto& br : i->elifs)
                for (auto& x : br.body) walk(x.get());
            if (i->hasElse)
                for (auto& x : i->elseBody) walk(x.get());
            return;
        }
        if (auto* w = dynamic_cast<WhileStmt*>(st)) {
            for (auto& x : w->body) walk(x.get());
            return;
        }
        if (auto* d = dynamic_cast<DoWhileStmt*>(st)) {
            for (auto& x : d->body) walk(x.get());
            return;
        }
        if (auto* f = dynamic_cast<ForNumStmt*>(st)) {
            for (auto& x : f->body) walk(x.get());
            return;
        }
        if (auto* f = dynamic_cast<ForInStmt*>(st)) {
            for (auto& x : f->body) walk(x.get());
            return;
        }
        if (auto* t = dynamic_cast<TryStmt*>(st)) {
            for (auto& x : t->body) walk(x.get());
            for (auto& x : t->catchBody) walk(x.get());
            for (auto& x : t->finallyBody) walk(x.get());
            return;
        }
        // 其余语句：递归进它们携带的表达式（return f = func(){...} 这类）
        if (auto* r = dynamic_cast<ReturnStmt*>(st)) {
            for (auto& e : r->values) walkE(e.get());
            return;
        }
        if (auto* e2 = dynamic_cast<ExprStmt*>(st)) { walkE(e2->e.get()); return; }
        if (auto* a = dynamic_cast<Assign*>(st)) {
            for (auto& e : a->values) walkE(e.get());
            return;
        }
        if (auto* l = dynamic_cast<LocalDecl*>(st)) {
            for (auto& e : l->values) walkE(e.get());
            return;
        }
    };
    // 表达式里的 Lambda 也要算（比如 local f = func(){...}）
    for (auto& st : body) walk(st.get());
    for (auto& n : extra) mine.insert(n);
    for (auto& n : g.upNames) mine.insert(n);       // 上值名也可能被内层再捕获
    for (auto& pr : nests) {
        auto fv = freeVarsOf(*pr.first, *pr.second, g, &mine);
        for (auto& n : fv) if (mine.count(n)) g.boxedNames.insert(n);
    }
}

inline void prescanStmts(CppGen& g, const std::vector<aeast::StmtP>& v) {
    for (const aeast::StmtP& st : v) prescan(g, st.get());
}
inline void prescan(CppGen& g, aeast::Stmt* s) {
    using namespace aeast;
    if (!s) return;
    if (auto* b = dynamic_cast<Block*>(s))      { g.push(); prescanStmts(g, b->stmts); g.pop(); return; }
    if (auto* l = dynamic_cast<LocalDecl*>(s))  { for (auto& n : l->names) if (!n.empty()) g.declare(n); return; }
    if (auto* i = dynamic_cast<IfStmt*>(s)) {
        g.push(); prescanStmts(g, i->primary.body); g.pop();
        for (auto& b : i->elifs) { g.push(); prescanStmts(g, b.body); g.pop(); }
        if (i->hasElse) { g.push(); prescanStmts(g, i->elseBody); g.pop(); }
        return;
    }
    if (auto* w = dynamic_cast<WhileStmt*>(s)) { g.push(); prescanStmts(g, w->body); g.pop(); return; }
    if (auto* d = dynamic_cast<DoWhileStmt*>(s)) { g.push(); prescanStmts(g, d->body); g.pop(); return; }
    if (auto* t = dynamic_cast<TryStmt*>(s)) {
        g.push(); prescanStmts(g, t->body); g.pop();
        if (t->hasCatch) { g.push(); if (!t->catchVar.empty()) g.declare(t->catchVar);
                           prescanStmts(g, t->catchBody); g.pop(); }
        if (t->hasFinally) { g.push(); prescanStmts(g, t->finallyBody); g.pop(); }
        return;
    }
    // 隐藏控制槽（$ 开头，不可能与用户名冲突）：循环边界 / 键列表 / 索引
    // 必须放在局部区—— 临时区会被循环体内的 resetTmp 覆盖掉。
    if (dynamic_cast<FuncDef*>(s)) { g.declare(static_cast<FuncDef*>(s)->name); return; }
    if (auto* f = dynamic_cast<ForNumStmt*>(s)) {
        g.push(); g.declare(f->name);
        { std::string u = "$L" + std::to_string(++g.lblCnt) + "_";
          g.declare(u+"s"); g.declare(u+"e"); if (f->stepv) g.declare(u+"st"); }
        prescanStmts(g, f->body); g.pop(); return;
    }
    if (auto* f = dynamic_cast<ForInStmt*>(s)) {
        // 单变量 for v in t：v 是值（与 VM 一致），双变量才是 键,值
        g.push(); g.declare(f->kName); if (!f->vName.empty()) g.declare(f->vName);
        { std::string u = "$L" + std::to_string(++g.lblCnt) + "_";
          g.declare(u+"ks"); g.declare(u+"i"); g.declare(u+"n"); g.declare(u+"k"); g.declare(u+"src"); }
        prescanStmts(g, f->body); g.pop(); return;
    }
}

// 表达式
// 推导：编译期展开成等价的嵌套循环（与字节码后端同一套语义）
inline void genComprLevel(CppGen& g, aeast::Comprehension* c, size_t i, int rSlot, int idxSlot);

inline int genCompr(CppGen& g, aeast::Comprehension* c) {
    // 推导要在表达式求值过程中declare 变量，而参数槽已在临时区分配。
    // 把局部槽顶到临时区之上，否则后续 declare 会覆盖已算好的参数。
    if (g.slotTop < g.tmpTop) g.slotTop = g.tmpTop;
    g.push();                      // 推导变量不泄漏到外层作用域
    int rSlot = g.declare("$cr");
    int idxSlot = g.declare("$ci");
    g.line("{");
    g.ind += "    ";
    g.line(g.ref(rSlot) + " = Value(vm->allocTable());");
    g.line(g.ref(idxSlot) + " = Value((int64_t)1);");
    genComprLevel(g, c, 0, rSlot, idxSlot);
    g.dedent();
    g.line("}");
    g.pop();
    int t = g.allocTmp();
    g.line(g.ref(t) + " = " + g.ref(rSlot) + ";");
    return t;
}

inline void genComprLevel(CppGen& g, aeast::Comprehension* c, size_t i, int rSlot, int idxSlot) {
    using namespace aeast;
    if (i >= c->items.size()) {
        int key, val;
        if (c->isDict) {
            key = c->key   ? genExpr(g, c->key.get())   : g.allocTmp();
            val = c->value ? genExpr(g, c->value.get()) : g.allocTmp();
        } else {
            key = idxSlot;
            val = c->value ? genExpr(g, c->value.get()) : g.allocTmp();
        }
        g.line("aeSetIndex(vm, " + g.ref(rSlot) + ", " + g.ref(key) + ", " + g.ref(val) + ");");
        if (!c->isDict)
            g.line(g.ref(idxSlot) + " = vm->doAdd(" + g.ref(idxSlot) + ", Value((int64_t)1));");
        g.resetTmp();
        return;
    }
    ComprItem& it = c->items[i];
    std::string top = g.newLbl("ae_ct"), end = g.newLbl("ae_ce"), cont = g.newLbl("ae_cc");
    if (it.isNumeric) {
        int iS = g.declare(it.nName);
        int eS = g.declare("$ce" + std::to_string(i));
        int stS = g.declare("$cs" + std::to_string(i));
        { auto bi = g.boxOfSlot.find(iS);
          if (bi != g.boxOfSlot.end())
              g.line("BX[" + std::to_string(bi->second) + "] = vm->allocBox();"); }
        { int x = genExpr(g, it.start.get()); g.line(g.ref(iS) + " = " + g.ref(x) + ";"); g.resetTmp(); }
        { int x = genExpr(g, it.stop.get());  g.line(g.ref(eS) + " = " + g.ref(x) + ";"); g.resetTmp(); }
        int stX;
        if (it.stepv) { stX = genExpr(g, it.stepv.get()); }
        else { stX = g.allocTmp(); g.line(g.ref(stX) + " = Value((int64_t)1);"); }
        g.line(g.ref(stS) + " = " + g.ref(stX) + ";");
        g.resetTmp();
        g.line(top + ": ;");
        g.line("if (aeStepNeg(" + g.ref(stS) + ")) {");
        g.ind += "    ";
        { int r = g.allocTmp();
          g.line(g.ref(r) + " = aeCmp(vm, 0x15, " + g.ref(iS) + ", " + g.ref(eS) + ");");
          g.line("if (vm->isFalse(" + g.ref(r) + ")) goto " + end + ";"); }
        g.dedent();
        g.line("} else {");
        g.ind += "    ";
        { int r = g.allocTmp();
          g.line(g.ref(r) + " = aeCmp(vm, 0x13, " + g.ref(iS) + ", " + g.ref(eS) + ");");
          g.line("if (vm->isFalse(" + g.ref(r) + ")) goto " + end + ";"); }
        g.dedent();
        g.line("}");
        if (it.cond) {
            int cv = genExpr(g, it.cond.get());
            g.line("if (vm->isFalse(" + g.ref(cv) + ")) goto " + cont + ";");
            g.resetTmp();
        }
        genComprLevel(g, c, i + 1, rSlot, idxSlot);
        g.line(cont + ": ;");
        { auto bi = g.boxOfSlot.find(iS);
          int nv = g.allocTmp();
          g.line(g.ref(nv) + " = vm->doAdd(" + g.ref(iS) + ", " + g.ref(stS) + ");");
          if (bi != g.boxOfSlot.end())
              g.line("BX[" + std::to_string(bi->second) + "] = vm->allocBox();");
          g.line(g.ref(iS) + " = " + g.ref(nv) + ";"); }
        g.line("goto " + top + ";");
        g.line(end + ": ;");
        g.resetTmp();
        return;
    }
    // for-in
    int itS  = g.declare("$cit" + std::to_string(i));   // 每层独立，否则嵌套互相覆盖
    int kyS  = g.declare("$ck" + std::to_string(i));
    int cnS  = g.declare("$cn" + std::to_string(i));
    int cuS  = g.declare("$cu" + std::to_string(i));
    { int x = genExpr(g, it.iter.get()); g.line(g.ref(itS) + " = " + g.ref(x) + ";"); g.resetTmp(); }
    { int x = g.allocTmp();
      g.line(g.ref(x) + " = aeKeys(vm, " + g.ref(itS) + ");");
      g.line(g.ref(kyS) + " = " + g.ref(x) + ";"); g.resetTmp(); }
    { int x = g.allocTmp();
      g.line(g.ref(x) + " = aeCallBuiltin(vm, 3, &" + g.ref(kyS) + ", 1, \"" + g.srcFile + "\", 0, 0);");
      g.line(g.ref(cnS) + " = " + g.ref(x) + ";"); g.resetTmp(); }
    g.line(g.ref(cuS) + " = Value((int64_t)1);");
    int kS = g.declare(it.kName);
    int vS = it.vName.empty() ? -1 : g.declare(it.vName);
    g.line(top + ": ;");
    { int r = g.allocTmp();
      g.line(g.ref(r) + " = aeCmp(vm, 0x13, " + g.ref(cuS) + ", " + g.ref(cnS) + ");");
      g.line("if (vm->isFalse(" + g.ref(r) + ")) goto " + end + ";"); }
    if (vS < 0) {
        int a = g.allocTmp(), b2 = g.allocTmp();
        g.line(g.ref(a) + " = aeIndex(vm, " + g.ref(kyS) + ", " + g.ref(cuS) + ");");
        g.line(g.ref(b2) + " = aeIndex(vm, " + g.ref(itS) + ", " + g.ref(a) + ");");
        auto bk = g.boxOfSlot.find(kS);
        if (bk != g.boxOfSlot.end()) g.line("BX[" + std::to_string(bk->second) + "] = vm->allocBox();");
        g.line(g.ref(kS) + " = " + g.ref(b2) + ";");
    } else {
        int a = g.allocTmp(), b2 = g.allocTmp();
        g.line(g.ref(a) + " = aeIndex(vm, " + g.ref(kyS) + ", " + g.ref(cuS) + ");");
        g.line(g.ref(kS) + " = " + g.ref(a) + ";");
        g.line(g.ref(b2) + " = aeIndex(vm, " + g.ref(itS) + ", " + g.ref(a) + ");");
        auto bk = g.boxOfSlot.find(kS), bv = g.boxOfSlot.find(vS);
        if (bk != g.boxOfSlot.end()) g.line("BX[" + std::to_string(bk->second) + "] = vm->allocBox();");
        if (bv != g.boxOfSlot.end()) g.line("BX[" + std::to_string(bv->second) + "] = vm->allocBox();");
        g.line(g.ref(vS) + " = " + g.ref(b2) + ";");
    }
    g.resetTmp();
    if (it.cond) {
        int cv = genExpr(g, it.cond.get());
        g.line("if (vm->isFalse(" + g.ref(cv) + ")) goto " + cont + ";");
        g.resetTmp();
    }
    genComprLevel(g, c, i + 1, rSlot, idxSlot);
    g.line(cont + ": ;");
    { int r = g.allocTmp();
      g.line(g.ref(r) + " = vm->doAdd(" + g.ref(cuS) + ", Value((int64_t)1));");
      g.line(g.ref(cuS) + " = " + g.ref(r) + ";"); }
    g.line("goto " + top + ";");
    g.line(end + ": ;");
    g.resetTmp();
}

inline int genExpr(CppGen& g, aeast::Expr* e) {
    using namespace aeast;
    if (!e) { int t = g.allocTmp(); g.line(g.ref(t) + " = Value();"); return t; }

    if (auto* n = dynamic_cast<NumLit*>(e)) {
        int t = g.allocTmp();
        if (n->isFloat) {
            char fb[40]; snprintf(fb, sizeof fb, "%.17g", n->d);   // 保留完整精度
            g.line(g.ref(t) + " = Value((double)" + fb + ");");
        }
        else            g.line(g.ref(t) + " = Value((int64_t)" + std::to_string(n->i) + "LL);");
        return t;
    }
    if (auto* s2 = dynamic_cast<StrLit*>(e)) {
        int t = g.allocTmp();
        g.line(g.ref(t) + " = aeStr(vm, \"" + cppEsc(s2->s) + "\");");
        return t;
    }
    if (auto* b = dynamic_cast<BoolLit*>(e)) {
        int t = g.allocTmp();
        g.line(g.ref(t) + " = Value(" + (b->v ? "true" : "false") + ");");
        return t;
    }
    if (dynamic_cast<NullLit*>(e)) { int t = g.allocTmp(); g.line(g.ref(t) + " = Value();"); return t; }

    if (auto* f = dynamic_cast<Field*>(e)) {
        if (auto* oid = dynamic_cast<Ident*>(f->obj.get()))
            if (modTable().count(oid->name)) {
                auto& vi = modTable()[oid->name].varIdx;
                auto it = vi.find(f->name);
                if (it != vi.end()) {
                    int t = g.allocTmp();
                    g.line(g.ref(t) + " = MODV_" + oid->name + "[" + std::to_string(it->second) + "];");
                    return t;
                }
                if (modTable()[oid->name].funcs.count(f->name)) {   // 模块函数当值
                    int t = g.allocTmp();
                    g.line(g.ref(t) + " = aeFnByName(vm, \"" + oid->name + "." + f->name + "\");");
                    return t;
                }
            }
    }

    if (auto* id = dynamic_cast<Ident*>(e)) {
        if (!g.curMod.empty() && modTable().count(g.curMod)) {
            auto& vi = modTable()[g.curMod].varIdx;
            auto it = vi.find(id->name);
            if (it != vi.end()) {
                int t = g.allocTmp();
                g.line(g.ref(t) + " = MODV_" + g.curMod + "[" + std::to_string(it->second) + "];");
                return t;
            }
        }
        int slot;
        if (g.lookup(id->name, slot)) { g.lastSlot = slot; return slot; }
        // 上值：从闭包继承来的（UP[j]->get() 是 Value&，可作左值）
        auto ui = g.upIdx.find(id->name);
        if (ui != g.upIdx.end()) { g.lastUp = ui->second; return -1000 - ui->second; }
        int gi = g.globalSlot(id->name);
        if (gi >= 0) { int t = g.allocTmp(); g.line(g.ref(t) + " = G[" + std::to_string(gi) + "];"); return t; }
        // 顶层函数名当值用（apply(add3, 1) / t.fn = add3）
        if (g.funcNames.count(id->name)) {
            int t = g.allocTmp();
            g.line(g.ref(t) + " = aeFnByName(vm, \"" + id->name + "\");");
            return t;
        }
        throw std::runtime_error("AOT: 未定义的变量 '" + id->name + "'");
    }

    if (auto* u = dynamic_cast<Unary*>(e)) {
        int a = genExpr(g, u->e.get());
        int t = g.allocTmp();
        if (u->op == TK_MINUS)      g.line(g.ref(t) + " = aeNeg(vm, " + g.ref(a) + ");");
        else if (u->op == TK_NOT)   g.line(g.ref(t) + " = Value(vm->isFalse(" + g.ref(a) + "));");
        else if (u->op == TK_TILDE) {
            int v = genExpr(g, u->e.get());
            int t = g.allocTmp();
            g.line(g.ref(t) + " = aeBitNot(" + g.ref(v) + ");");
            return t;
        }
        else throw std::runtime_error("AOT: 未支持的一元运算符");
        return t;
    }

    if (auto* b = dynamic_cast<Binary*>(e)) {
        if (b->op == TK_OR || b->op == TK_AND) {
            int t = g.allocTmp();
            int a = genExpr(g, b->l.get());
            g.line(g.ref(t) + " = " + g.ref(a) + ";");
            std::string cond = (b->op == TK_OR) ? "vm->isFalse" : "!vm->isFalse";
            g.line("if (" + cond + "(" + g.ref(t) + ")) {");
            g.ind += "    ";
            int r = genExpr(g, b->r.get());
            g.line(g.ref(t) + " = " + g.ref(r) + ";");
            g.dedent();
            g.line("}");
            return t;
        }
        int a = genExpr(g, b->l.get());
        int c = genExpr(g, b->r.get());
        int t = g.allocTmp();
        std::string L = g.ref(a), R = g.ref(c), T = g.ref(t);
        switch (b->op) {
            case TK_PLUS:  g.line(T + " = aeBin(vm, \"doAdd\", " + L + ", " + R + ", " + "\"" + g.srcFile + "\", " + (b->endLine ? std::to_string(b->endLine) : std::to_string(b->line)) + ", " + (b->endCol ? std::to_string(b->endCol) : std::to_string(b->col)) + ");"); break;
            case TK_MINUS: g.line(T + " = aeBin(vm, \"doSub\", " + L + ", " + R + ", " + "\"" + g.srcFile + "\", " + (b->endLine ? std::to_string(b->endLine) : std::to_string(b->line)) + ", " + (b->endCol ? std::to_string(b->endCol) : std::to_string(b->col)) + ");"); break;
            case TK_MUL:   g.line(T + " = aeBin(vm, \"doMul\", " + L + ", " + R + ", " + "\"" + g.srcFile + "\", " + (b->endLine ? std::to_string(b->endLine) : std::to_string(b->line)) + ", " + (b->endCol ? std::to_string(b->endCol) : std::to_string(b->col)) + ");"); break;
            case TK_DIV:   g.line(T + " = aeBin(vm, \"doDiv\", " + L + ", " + R + ", " + "\"" + g.srcFile + "\", " + (b->endLine ? std::to_string(b->endLine) : std::to_string(b->line)) + ", " + (b->endCol ? std::to_string(b->endCol) : std::to_string(b->col)) + ");"); break;
            case TK_MOD:   g.line(T + " = aeBin(vm, \"doMod\", " + L + ", " + R + ", " + "\"" + g.srcFile + "\", " + (b->endLine ? std::to_string(b->endLine) : std::to_string(b->line)) + ", " + (b->endCol ? std::to_string(b->endCol) : std::to_string(b->col)) + ");"); break;
            case TK_POW:   g.line(T + " = aePow(vm, " + L + ", " + R + ");"); break;
            case TK_AMP:   g.line(T + " = aeBit(vm, \"&\", 0, " + L + ", " + R + ");"); break;
            case TK_PIPE:  g.line(T + " = aeBit(vm, \"|\", 1, " + L + ", " + R + ");"); break;
            case TK_CARET: g.line(T + " = aeBit(vm, \"^\", 2, " + L + ", " + R + ");"); break;
            case TK_SHL:   g.line(T + " = aeBit(vm, \"<<\", 3, " + L + ", " + R + ");"); break;
            case TK_SHR:   g.line(T + " = aeBit(vm, \">>\", 4, " + L + ", " + R + ");"); break;
            default: throw std::runtime_error("AOT: 未支持的二元运算符");
        }
        return t;
    }

    // 链式比较：a<b<c ≡ (a<b) and (b<c)，中间操作数只求值一次
    if (auto* ch = dynamic_cast<Chain*>(e)) {
        int t = g.allocTmp();
        std::string done = g.newLbl("ae_ch");
        int prev = genExpr(g, ch->operands[0].get());
        for (size_t i = 0; i < ch->ops.size(); i++) {
            if (ch->ops[i] == TK_NOT) throw std::runtime_error("AOT: 未支持的链式比较运算符");
            int cur = genExpr(g, ch->operands[i + 1].get());
            int r = g.allocTmp();
            if (ch->ops[i] == TK_IN) {
                g.line(g.ref(r) + " = aeIn(vm, " + g.ref(prev) + ", " + g.ref(cur) + ");");
            } else {
            int op = 0x10;
            switch (ch->ops[i]) {
                case TK_EQ: op = 0x10; break;
                case TK_NEQ: op = 0x11; break;
                case TK_LT: op = 0x12; break;
                case TK_LE: op = 0x13; break;
                case TK_GT: op = 0x14; break;
                case TK_GE: op = 0x15; break;
                default: throw std::runtime_error("AOT: 未支持的链式比较运算符");
            }
            char opb[16]; snprintf(opb, sizeof opb, "0x%02X", op);
            g.line(g.ref(r) + " = aeCmp(vm, " + opb + ", " + g.ref(prev) + ", " + g.ref(cur) + ");");
            }
            if (i < ch->negates.size() && ch->negates[i])
                g.line("if (!vm->isFalse(" + g.ref(r) + ")) { " + g.ref(t) + " = Value(false); goto " + done + "; }");
            else
                g.line("if (vm->isFalse(" + g.ref(r) + ")) { " + g.ref(t) + " = Value(false); goto " + done + "; }");
            prev = cur;
        }
        g.line(g.ref(t) + " = Value(true);");
        g.line(done + ": ;");
        return t;
    }

    if (auto* f = dynamic_cast<Field*>(e)) {
        int o = genExpr(g, f->obj.get());
        int t = g.allocTmp();
        g.line(g.ref(t) + " = aeIndex(vm, " + g.ref(o) + ", aeStr(vm, \"" + cppEsc(f->name) + "\"));");
        return t;
    }
    if (auto* ix = dynamic_cast<Index*>(e)) {
        int o = genExpr(g, ix->obj.get());
        int k = genExpr(g, ix->key.get());
        int t = g.allocTmp();
        g.line(g.ref(t) + " = aeIndex(vm, " + g.ref(o) + ", " + g.ref(k) + ");");
        return t;
    }
    if (auto* tl = dynamic_cast<TableLit*>(e)) {
        int t = g.allocTmp();
        g.line(g.ref(t) + " = Value(vm->allocTable());");
        for (size_t i = 0; i < tl->items.size(); i++) {
            auto& it = tl->items[i];
            int v = genExpr(g, it.value.get());
            if (it.key) {
                int k = genExpr(g, it.key.get());
                g.line("aeSetIndex(vm, " + g.ref(t) + ", " + g.ref(k) + ", " + g.ref(v) + ");");
            } else {
                g.line("aeSetIndex(vm, " + g.ref(t) + ", Value((int64_t)" + std::to_string(i + 1) + "LL), " + g.ref(v) + ");");
            }
        }
        return t;
    }
    if (auto* L = dynamic_cast<Lambda*>(e)) return genLambda(g, L);

    if (auto* c = dynamic_cast<Call*>(e)) {
        // 原生库调用：mod.fn(args)
        if (auto* f = dynamic_cast<Field*>(c->callee.get()))
            if (auto* id = dynamic_cast<Ident*>(f->obj.get()))
                if (g.nativeMods.count(id->name)) {
                    int t = g.allocTmp();
                    int base = g.allocTmp();
                    for (size_t i = 1; i < c->args.size(); i++) g.allocTmp();
                    for (size_t i = 0; i < c->args.size(); i++) {
                        int a = genExpr(g, c->args[i].get());
                        if (a != base + (int)i) g.line(g.ref(base + i) + " = " + g.ref(a) + ";");
                    }
                    g.line(g.ref(t) + " = aeCallNative(vm, \"" + id->name + "." + f->name +
                           "\", &" + g.ref(base) + ", " + std::to_string(c->args.size()) + ");");
                    return t;
                }
        std::string fname;
        bool viaVar = false; int vslot = 0;
        // 模块函数调用：json.parse(...) 或模块内互调 helper(...)
        {
            std::string mod = g.curMod, fn;
            if (auto* f2 = dynamic_cast<Field*>(c->callee.get()))
                if (auto* oid = dynamic_cast<Ident*>(f2->obj.get()))
                    if (modTable().count(oid->name)) { mod = oid->name; fn = f2->name; }
            if (fn.empty()) if (auto* id2 = dynamic_cast<Ident*>(c->callee.get())) fn = id2->name;
            if (!fn.empty() && !mod.empty() && modTable().count(mod) &&
                modTable()[mod].funcs.count(fn)) {
                int t = g.allocTmp();
                int base = g.allocTmp();
                for (size_t i = 1; i < c->args.size(); i++) g.allocTmp();
                for (size_t i = 0; i < c->args.size(); i++) {
                    int a = genExpr(g, c->args[i].get());
                    if (a != base + (int)i) g.line(g.ref(base + i) + " = " + g.ref(a) + ";");
                }
                g.line("for (int i = 0; i < AE_MAX_RET; i++) RV[i] = Value();");
                g.line("ae_m_" + mod + "_" + fn + "(vm, " + std::to_string(c->args.size()) +
                       ", &" + g.ref(base) + ", RV, nullptr);");
                g.line(g.ref(t) + " = RV[0];");
                return t;
            }
        }
        if (auto* id = dynamic_cast<Ident*>(c->callee.get())) {
            fname = id->name;
            int slot;
            if (g.lookup(fname, slot)) { viaVar = true; vslot = slot; }
            else if (g.upIdx.count(fname)) { viaVar = true; vslot = -1000 - g.upIdx.at(fname); }
        } else {
            // 任意表达式作调用目标（t.f() / 表里的函数值）：先求值，再动态调用
            vslot = genExpr(g, c->callee.get());
            viaVar = true;
        }
        int base = g.allocTmp();
        for (size_t i = 1; i < c->args.size(); i++) g.allocTmp();
        for (size_t i = 0; i < c->args.size(); i++) {
            int a = genExpr(g, c->args[i].get());
            if (a != (int)base + (int)i) g.line(g.ref(base + i) + " = " + g.ref(a) + ";");
        }
        int t = g.allocTmp();
        if (viaVar) {
            g.line("for (int i = 0; i < AE_MAX_RET; i++) RV[i] = Value();");
            g.line(g.ref(t) + " = aeCallValue(vm, " + g.ref(vslot) + ", " +
                   std::to_string(c->args.size()) + ", &" + g.ref(base) + ");");
            return t;
        }
        int bid = cppBuiltinId(fname);
        if (bid >= 0)
            g.line(g.ref(t) + " = aeCallBuiltin(vm, " + std::to_string(bid) + ", &" + g.ref(base) + ", " +
                   std::to_string(c->args.size()) + ", \"" + g.srcFile + "\", " +
                   std::to_string(c->line) + ", " + std::to_string(c->col) + ");");
        else if (g.funcNames.count(fname)) {
            g.line("for (int i = 0; i < AE_MAX_RET; i++) RV[i] = Value();");
            g.line("ae_f_" + fname + "(vm, " + std::to_string(c->args.size()) + ", &" + g.ref(base) + ", RV, nullptr);");
            g.line(g.ref(t) + " = RV[0];");
        }
        else if (g.nativeFuncs.count(fname)) {
            g.line(g.ref(t) + " = aeCallNative(vm, \"" + g.nativeFuncs.at(fname) +
                   "\", &" + g.ref(base) + ", " + std::to_string(c->args.size()) + ");");
        }
        else {
            // 全局槽里存的函数值（g = dbl; g(5)）→ 动态调用
            int gi = g.globalSlot(fname);
            if (gi < 0) throw std::runtime_error("AOT: 未定义的函数 '" + fname + "'");
            int fs = g.allocTmp();
            g.line(g.ref(fs) + " = G[" + std::to_string(gi) + "];");
            g.line(g.ref(t) + " = aeCallValue(vm, " + g.ref(fs) + ", " +
                   std::to_string(c->args.size()) + ", &" + g.ref(base) + ");");
        }
        return t;
    }
    if (auto* cp = dynamic_cast<Comprehension*>(e)) return genCompr(g, cp);

    if (auto* sl = dynamic_cast<Slice*>(e)) {
        int o = genExpr(g, sl->obj.get());
        int a = genExpr(g, sl->start.get());
        int b = genExpr(g, sl->end.get());
        int st = genExpr(g, sl->step.get());
        int t = g.allocTmp();
        g.line(g.ref(t) + " = aeSlice(vm, " + g.ref(o) + ", " + g.ref(a) + ", " +
               g.ref(b) + ", " + g.ref(st) + ");");
        return t;
    }
    if (auto* tn = dynamic_cast<Ternary*>(e)) {
        int t = g.allocTmp();
        int c = genExpr(g, tn->c.get());
        std::string els = g.newLbl("ae_te"), done = g.newLbl("ae_td");
        g.line("if (vm->isFalse(" + g.ref(c) + ")) goto " + els + ";");
        { int a = genExpr(g, tn->a.get());
          if (a != t) g.line(g.ref(t) + " = " + g.ref(a) + ";"); }
        g.line("goto " + done + ";");
        g.line(els + ": ;");
        { int b = genExpr(g, tn->b.get());
          if (b != t) g.line(g.ref(t) + " = " + g.ref(b) + ";"); }
        g.line(done + ": ;");
        return t;
    }
    throw std::runtime_error("AOT: 未支持的表达式");
}

// 赋值目标
inline void genTarget(CppGen& g, aeast::Expr* tgt, std::string& outLhs) {
    using namespace aeast;
    if (auto* id = dynamic_cast<Ident*>(tgt)) {
        int slot;
        if (g.lookup(id->name, slot)) { outLhs = g.ref(slot); return; }
        auto ui = g.upIdx.find(id->name);
        if (ui != g.upIdx.end()) { outLhs = "UP[" + std::to_string(ui->second) + "]->get()"; return; }
        int gi = g.globalSlot(id->name);
        if (gi >= 0) { outLhs = "G[" + std::to_string(gi) + "]"; return; }
        throw std::runtime_error("AOT: 赋值给未声明的变量 '" + id->name + "'");
    }
    if (auto* ix = dynamic_cast<Index*>(tgt)) {
        int o = genExpr(g, ix->obj.get());
        int k = genExpr(g, ix->key.get());
        outLhs = "@IDX:" + std::to_string(o) + ":" + std::to_string(k);
        return;
    }
    if (auto* f = dynamic_cast<Field*>(tgt)) {
        int o = genExpr(g, f->obj.get());
        int k = g.allocTmp();
        g.line(g.ref(k) + " = aeStr(vm, \"" + cppEsc(f->name) + "\");");
        outLhs = "@IDX:" + std::to_string(o) + ":" + std::to_string(k);
        return;
    }
    throw std::runtime_error("AOT: 未支持的赋值目标");
}
inline void genStoreTo(CppGen& g, const std::string& lhs, const std::string& rhs) {
    if (lhs.size() > 5 && lhs.substr(0, 5) == "@IDX:") {
        size_t p = lhs.find(':', 5);
        int o = std::stoi(lhs.substr(5, p - 5));
        int k = std::stoi(lhs.substr(p + 1));
        g.line("aeSetIndex(vm, " + g.ref(o) + ", " + g.ref(k) + ", " + rhs + ");");
    } else {
        g.line(lhs + " = " + rhs + ";");
    }
}

// 语句
inline bool blockHasDefer(const std::vector<aeast::StmtP>& v) {
    using namespace aeast;
    for (auto& st : v) {
        if (dynamic_cast<DeferStmt*>(st.get())) return true;
        if (auto* b = dynamic_cast<Block*>(st.get())) if (blockHasDefer(b->stmts)) return true;
        if (auto* i = dynamic_cast<IfStmt*>(st.get())) {
            if (blockHasDefer(i->primary.body)) return true;
            for (auto& b : i->elifs) if (blockHasDefer(b.body)) return true;
            if (i->hasElse && blockHasDefer(i->elseBody)) return true;
        }
        if (auto* w = dynamic_cast<WhileStmt*>(st.get())) if (blockHasDefer(w->body)) return true;
        if (auto* f = dynamic_cast<ForNumStmt*>(st.get())) if (blockHasDefer(f->body)) return true;
        if (auto* f = dynamic_cast<ForInStmt*>(st.get())) if (blockHasDefer(f->body)) return true;
        if (auto* t = dynamic_cast<TryStmt*>(st.get())) {
            if (blockHasDefer(t->body)) return true;
            if (t->hasCatch && blockHasDefer(t->catchBody)) return true;
            if (t->hasFinally && blockHasDefer(t->finallyBody)) return true;
        }
    }
    return false;
}
// 块级 defer 作用域：defer 必须在本块退出时跑（不是函数退出时）
inline void genBlockScoped(CppGen& g, const std::vector<aeast::StmtP>& v) {
    if (!blockHasDefer(v)) { g.push(); genStmts(g, v); g.pop(); return; }
    g.line("{");
    g.ind += "    ";
    g.line("AeDeferScope _bds(vm); AeDsReg _bdsreg(&_bds);");
    g.push(); genStmts(g, v); g.pop();
    g.dedent();
    g.line("}");
}
inline void genStmts(CppGen& g, const std::vector<aeast::StmtP>& v) {
    for (const aeast::StmtP& st : v) genStmt(g, st.get());
}
// catch 子句：带位置的 AeErrPos 优先，其余 std::exception 兜底
inline void genCatchClauses(CppGen& g, aeast::TryStmt* t, bool innerNested) {
    using namespace aeast;
    std::string eslotRef;
    g.push();
    int eslot = -1;
    if (!t->catchVar.empty()) {
        CppGen cb; cb.globals = g.globals; cb.nativeMods = g.nativeMods; cb.nativeFuncs = g.nativeFuncs; cb.funcNames = g.funcNames; cb.curMod = g.curMod;
        computeBoxed(cb, t->catchBody);
        for (auto& n : cb.boxedNames) g.boxedNames.insert(n);
        eslot = g.declare(t->catchVar);
    }
    auto emitBox = [&]() {
        if (eslot < 0) return;
        auto bi = g.boxOfSlot.find(eslot);
        if (bi != g.boxOfSlot.end())
            g.line("BX[" + std::to_string(bi->second) + "] = vm->allocBox();");
    };
    g.line("catch (const ExitException&) { throw; }");
    g.line("catch (const AeErrPos& _p) {");
    g.ind += "    ";
    emitBox();
    if (eslot >= 0)
        g.line(g.ref(eslot) + " = aeErrTable(vm, _p.msg, _p.file, _p.line, _p.col, _p.stack);");
    genStmts(g, t->catchBody);
    g.dedent();
    g.line("} catch (const std::exception& _e) {");
    g.ind += "    ";
    emitBox();
    if (eslot >= 0)
        g.line(g.ref(eslot) + " = aeErrTable(vm, _e.what(), \"" + g.srcFile + "\", " +
               std::to_string(t->line) + ", " + std::to_string(t->col) + ", aeFramesText(vm));");
    genStmts(g, t->catchBody);
    g.dedent();
    g.line("}");
    g.pop();
    (void)innerNested;
}

inline void genStmt(CppGen& g, aeast::Stmt* s) {
    using namespace aeast;
    if (!s) return;
    if (auto* b = dynamic_cast<Block*>(s)) { genBlockScoped(g, b->stmts); return; }
    if (auto* e = dynamic_cast<ExprStmt*>(s)) { genExpr(g, e->e.get()); g.resetTmp(); return; }

    if (auto* l = dynamic_cast<LocalDecl*>(s)) {
        if (!g.modInitOf.empty()) {
            for (size_t i = 0; i < l->names.size(); i++) {
                std::string dst = "MODV_" + g.modInitOf + "[" +
                    std::to_string(modTable()[g.modInitOf].varIdx[l->names[i]]) + "]";
                if (i < l->values.size() && l->values[i]) {
                    int v = genExpr(g, l->values[i].get());
                    g.line(dst + " = " + g.ref(v) + ";");
                } else g.line(dst + " = Value();");
                g.resetTmp();
            }
            return;
        }
        // 多返回值：右侧是单个用户函数调用时，按 RV[i] 逐个接住
        bool fromCall = false;
        if (l->names.size() > 1 && l->values.size() == 1 && l->values[0]) {
            if (auto* c = dynamic_cast<Call*>(l->values[0].get()))
                if (auto* id = dynamic_cast<Ident*>(c->callee.get()))
                    fromCall = g.funcNames.count(id->name) != 0;
        }
        std::vector<int> slots;
        for (auto& n : l->names) {
            int sl = g.declare(n);
            slots.push_back(sl);
            auto bi = g.boxOfSlot.find(sl);
            // 被捕获的变量每次声明都换一个盒子 —— 循环里每轮闭包各自独立
            if (bi != g.boxOfSlot.end()) g.line("BX[" + std::to_string(bi->second) + "] = vm->allocBox();");
            g.line(g.ref(sl) + " = Value();");
        }
        if (fromCall) {
            genExpr(g, l->values[0].get());
            for (size_t i = 0; i < slots.size(); i++)
                g.line(g.ref(slots[i]) + " = RV[" + std::to_string(i) + "];");
        } else {
            for (size_t i = 0; i < slots.size(); i++) {
                if (i < l->values.size() && l->values[i]) {
                    int v = genExpr(g, l->values[i].get());
                    g.line(g.ref(slots[i]) + " = " + g.ref(v) + ";");
                }
            }
        }
        g.resetTmp();
        return;
    }

    if (auto* a = dynamic_cast<Assign*>(s)) {
        if (!g.modInitOf.empty()) {
            for (size_t i = 0; i < a->targets.size(); i++) {
                auto* id = dynamic_cast<Ident*>(a->targets[i].get());
                if (!id || i >= a->values.size()) continue;
                std::string dst = "MODV_" + g.modInitOf + "[" +
                    std::to_string(modTable()[g.modInitOf].varIdx[id->name]) + "]";
                int v = genExpr(g, a->values[i].get());
                g.line(dst + " = " + g.ref(v) + ";");
                g.resetTmp();
            }
            return;
        }
        // 多赋值：a, b = b, a 先算右边全部值，再统一写左边（避免覆盖）
        std::vector<int> vs;
        bool fromCall = false;
        if (a->targets.size() > 1 && a->values.size() == 1 && a->values[0]) {
            if (auto* c = dynamic_cast<Call*>(a->values[0].get()))
                if (auto* id = dynamic_cast<Ident*>(c->callee.get()))
                    fromCall = g.funcNames.count(id->name) != 0;
        }
        if (fromCall) genExpr(g, a->values[0].get());
        else for (auto& e : a->values) {
            int v = genExpr(g, e.get());
            if (a->targets.size() > 1) {
                // 多赋值：右侧值必须拷进独立临时槽，否则写左边时会互相覆盖
                int c = g.allocTmp();
                g.line(g.ref(c) + " = " + g.ref(v) + ";");
                v = c;
            }
            vs.push_back(v);
        }
        std::vector<std::string> lhss;
        for (auto& t : a->targets) { std::string lhs; genTarget(g, t.get(), lhs); lhss.push_back(lhs); }
        for (size_t i = 0; i < lhss.size(); i++) {
            std::string rhs = fromCall ? ("RV[" + std::to_string(i) + "]")
                                       : (i < vs.size() ? g.ref(vs[i]) : "Value()");
            genStoreTo(g, lhss[i], rhs);
        }
        g.resetTmp();
        return;
    }

    if (auto* ca = dynamic_cast<CompoundAssign*>(s)) {
        std::string lhs; genTarget(g, ca->target.get(), lhs);
        int cur = g.allocTmp();
        if (lhs.size() > 5 && lhs.substr(0, 5) == "@IDX:") {
            size_t p = lhs.find(':', 5);
            int o = std::stoi(lhs.substr(5, p - 5)), k = std::stoi(lhs.substr(p + 1));
            g.line(g.ref(cur) + " = aeIndex(vm, " + g.ref(o) + ", " + g.ref(k) + ");");
        } else g.line(g.ref(cur) + " = " + lhs + ";");
        int rhs = genExpr(g, ca->value.get());
        int t = g.allocTmp();
        auto bin = [&](const char* fn) { g.line(g.ref(t) + " = vm->" + fn + "(" + g.ref(cur) + ", " + g.ref(rhs) + ");"); };
        switch (ca->op) {
            case TK_ADDEQ:  bin("doAdd");  break;
            case TK_SUBEQ:  bin("doSub");  break;
            case TK_MULEQ:  bin("doMul");  break;
            case TK_DIVEQ:  bin("doDiv");  break;
            default: throw std::runtime_error("AOT: 未支持的复合赋值运算符");
        }
        genStoreTo(g, lhs, g.ref(t));
        g.resetTmp();
        return;
    }

    if (auto* i = dynamic_cast<IfStmt*>(s)) {
        int c = genExpr(g, i->primary.cond.get());
        g.line("if (!vm->isFalse(" + g.ref(c) + ")) {");
        g.ind += "    "; g.resetTmp(); g.push(); genStmts(g, i->primary.body); g.pop();
        g.dedent();
        for (auto& br : i->elifs) {
            g.line("} else {");
            g.ind += "    ";
            int c2 = genExpr(g, br.cond.get());
            g.line("if (!vm->isFalse(" + g.ref(c2) + ")) {");
            g.ind += "    "; g.resetTmp(); g.push(); genStmts(g, br.body); g.pop();
            g.dedent(); g.dedent();
        }
        if (i->hasElse) {
            g.line("} else {");
            g.ind += "    "; g.resetTmp(); g.push(); genStmts(g, i->elseBody); g.pop();
            g.dedent();
        }
        // 每个 elif 都套了一层 `} else { ... if { ... }`，末尾要逐层闭合
        for (size_t k = 0; k < i->elifs.size(); k++) {
            if (g.ind.size() >= 4) g.dedent();
            g.line("}");
        }
        g.line("}");
        g.resetTmp();
        return;
    }

    if (auto* d = dynamic_cast<DoWhileStmt*>(s)) {
        std::string top = g.newLbl("ae_d"), brk = g.newLbl("ae_db"), cont = g.newLbl("ae_dc");
        g.brkLbl.push_back(brk); g.contLbl.push_back(cont);
        g.line(top + ": ;");
        g.push(); genStmts(g, d->body); g.pop();
        g.line(cont + ": ;");
        int c = genExpr(g, d->cond.get());
        g.line("if (!vm->isFalse(" + g.ref(c) + ")) goto " + top + ";");
        g.line(brk + ": ;");
        g.brkLbl.pop_back(); g.contLbl.pop_back();
        g.resetTmp();
        return;
    }

    if (auto* w = dynamic_cast<WhileStmt*>(s)) {
        std::string top = g.newLbl("ae_w"), brk = g.newLbl("ae_wb");
        g.brkLbl.push_back(brk); g.contLbl.push_back(top);
        g.line(top + ": ;");
        int c = genExpr(g, w->cond.get());
        g.line("if (vm->isFalse(" + g.ref(c) + ")) goto " + brk + ";");
        g.resetTmp();
        genBlockScoped(g, w->body);
        g.line("goto " + top + ";");
        g.line(brk + ": ;");
        g.brkLbl.pop_back(); g.contLbl.pop_back();
        g.resetTmp();
        return;
    }

    // 数值 for：for i = a, b [, step]
    if (auto* f = dynamic_cast<ForNumStmt*>(s)) {
        std::string top = g.newLbl("ae_f"), inc = g.newLbl("ae_fi"), brk = g.newLbl("ae_fb");
        g.push();
        int iv = g.declare(f->name);
        std::string U = "$L" + std::to_string(++g.lblCnt) + "_";
        int cs = g.declare(U+"s"), ce = g.declare(U+"e");
        int cst = f->stepv ? g.declare(U+"st") : -1;
        { int a = genExpr(g, f->start.get()); g.line(g.ref(cs) + " = " + g.ref(a) + ";"); }
        { int b = genExpr(g, f->end.get());   g.line(g.ref(ce) + " = " + g.ref(b) + ";"); }
        if (cst >= 0) { int st = genExpr(g, f->stepv.get()); g.line(g.ref(cst) + " = " + g.ref(st) + ";"); }
        g.line(g.ref(iv) + " = " + g.ref(cs) + ";");
        g.brkLbl.push_back(brk); g.contLbl.push_back(inc);
        g.line(top + ": ;");
        // 步长正负决定方向：+ 用 <=，- 用 >=
        if (cst >= 0) {
            // step < 0 → 判 i >= end(0x15)；否则判 i <= end(0x13)
            g.line("if (aeStepNeg(" + g.ref(cst) + ")) {");
            g.ind += "    ";
            g.line("if (vm->isFalse(aeCmp(vm, 0x15, " + g.ref(iv) + ", " + g.ref(ce) + "))) goto " + brk + ";");
            g.dedent();
            g.line("} else {");
            g.ind += "    ";
            g.line("if (vm->isFalse(aeCmp(vm, 0x13, " + g.ref(iv) + ", " + g.ref(ce) + "))) goto " + brk + ";");
            g.dedent();
            g.line("}");
        } else {
            g.line("if (vm->isFalse(aeCmp(vm, 0x13, " + g.ref(iv) + ", " + g.ref(ce) + "))) goto " + brk + ";");
        }
        g.resetTmp();
        genStmts(g, f->body);
        g.line(inc + ": ;");
        {
            auto bi = g.boxOfSlot.find(iv);
            int nv = g.allocTmp();
            g.line(g.ref(nv) + " = vm->doAdd(" + g.ref(iv) + ", " +
                   (cst >= 0 ? g.ref(cst) : "Value((int64_t)1)") + ");");
            if (bi != g.boxOfSlot.end())
                g.line("BX[" + std::to_string(bi->second) + "] = vm->allocBox();");
            g.line(g.ref(iv) + " = " + g.ref(nv) + ";");
        }
        g.line("goto " + top + ";");
        g.line(brk + ": ;");
        g.brkLbl.pop_back(); g.contLbl.pop_back();
        g.pop();
        g.resetTmp();
        return;
    }

    // for-in：先收集键列表（aeKeys），再按索引遍历
    if (auto* f = dynamic_cast<ForInStmt*>(s)) {
        std::string top = g.newLbl("ae_i"), inc = g.newLbl("ae_ii"), brk = g.newLbl("ae_ib");
        g.push();
        int srcT = genExpr(g, f->iter.get());
        int src = g.declare("$src");                 // 迭代表移到局部区，避免被 resetTmp 覆盖
        g.line(g.ref(src) + " = " + g.ref(srcT) + ";");
        std::string U = "$L" + std::to_string(++g.lblCnt) + "_";
        int ks = g.declare(U+"ks");
        g.line(g.ref(ks) + " = aeKeys(vm, " + g.ref(src) + ");");
        int ip = g.declare(U+"i");
        g.line(g.ref(ip) + " = Value((int64_t)0);");
        int nn = g.declare(U+"n");
        int kk = g.declare("$k");
        int kv = g.declare(f->kName);
        int vv = -1;
        if (!f->vName.empty()) vv = g.declare(f->vName);
        g.brkLbl.push_back(brk); g.contLbl.push_back(inc);
        g.line(top + ": ;");
        g.line(g.ref(nn) + " = Value((int64_t)lenOfTable(*" + g.ref(ks) + ".tab));");
        g.line(g.ref(ip) + " = vm->doAdd(" + g.ref(ip) + ", Value((int64_t)1));");
        g.line("if (vm->isFalse(aeCmp(vm, 0x13, " + g.ref(ip) + ", " + g.ref(nn) + "))) goto " + brk + ";");
        {
            auto bk = g.boxOfSlot.find(kv);
            if (bk != g.boxOfSlot.end())
                g.line("BX[" + std::to_string(bk->second) + "] = vm->allocBox();");
            if (vv >= 0) {
                auto bv = g.boxOfSlot.find(vv);
                if (bv != g.boxOfSlot.end())
                    g.line("BX[" + std::to_string(bv->second) + "] = vm->allocBox();");
            }
        }
        g.line(g.ref(kk) + " = aeIndex(vm, " + g.ref(ks) + ", " + g.ref(ip) + ");");
        if (vv >= 0) {
            g.line(g.ref(kv) + " = " + g.ref(kk) + ";");                              // 键
            g.line(g.ref(vv) + " = aeIndex(vm, " + g.ref(src) + ", " + g.ref(kk) + ");"); // 值
        } else {
            g.line(g.ref(kv) + " = aeIndex(vm, " + g.ref(src) + ", " + g.ref(kk) + ");"); // 单变量 = 值
        }
        g.resetTmp();
        genBlockScoped(g, f->body);
        g.line(inc + ": ;");
        g.line("goto " + top + ";");
        g.line(brk + ": ;");
        g.brkLbl.pop_back(); g.contLbl.pop_back();
        g.pop();
        g.resetTmp();
        return;
    }

    if (auto* r = dynamic_cast<ReturnStmt*>(s)) {
        std::vector<int> vs;
        for (auto& e : r->values) vs.push_back(genExpr(g, e.get()));
        for (size_t i = 0; i < vs.size(); i++) g.line("rv[" + std::to_string(i) + "] = " + g.ref(vs[i]) + ";");
        g.line("return " + std::to_string(vs.size()) + ";");
        return;
    }

    if (auto* b = dynamic_cast<BreakStmt*>(s)) {
        int lv = b->level < 1 ? 1 : b->level;
        if ((int)g.brkLbl.size() < lv)
            throw std::runtime_error("break " + std::to_string(lv) + " 超出了循环嵌套层数");
        // C++ 的 goto 跳出作用域会自动析构中间的 AeDeferScope，defer 不会被跳过
        g.line("goto " + g.brkLbl[g.brkLbl.size() - lv] + ";");
        return;
    }
    if (auto* c = dynamic_cast<ContinueStmt*>(s)) {
        int lv = c->level < 1 ? 1 : c->level;
        if ((int)g.contLbl.size() < lv)
            throw std::runtime_error("continue " + std::to_string(lv) + " 超出了循环嵌套层数");
        g.line("goto " + g.contLbl[g.contLbl.size() - lv] + ";");
        return;
    }
    // defer：作用域退出时执行（正常与异常路径都要跑，RAII 天然满足）
    if (auto* d = dynamic_cast<DeferStmt*>(s)) {
        if (!d->call) throw std::runtime_error("AOT: defer 后面必须是调用");
        g.declare("$d" + std::to_string(++g.lblCnt));
        if (auto* c = dynamic_cast<Call*>(d->call.get())) {
            // 参数立即求值（Go 语义）：先算好存进槽，退出时只管调用
            std::string fname; bool viaVar = false; int vslot = 0;
            if (auto* id = dynamic_cast<Ident*>(c->callee.get())) {
                fname = id->name;
                int sl;
                if (g.lookup(fname, sl)) { viaVar = true; vslot = sl; }
                else if (g.upIdx.count(fname)) { viaVar = true; vslot = -1000 - g.upIdx.at(fname); }
            } else { vslot = genExpr(g, c->callee.get()); viaVar = true; }
            int base = g.allocTmp();
            for (size_t i = 1; i < c->args.size(); i++) g.allocTmp();
            for (size_t i = 0; i < c->args.size(); i++) {
                int a = genExpr(g, c->args[i].get());
                if (a != (int)base + (int)i) g.line(g.ref(base + i) + " = " + g.ref(a) + ";");
            }
            int nb = (int)c->args.size();
            g.line("{ std::vector<Value> da;");
            for (int i = 0; i < nb; i++) g.line("  da.push_back(" + g.ref(base + i) + ");");
            g.line("  if (_ds_top()) _ds_top()->ds.push_back(aeMakeDefer(vm, " +
                   (viaVar ? g.ref(vslot) : ("aeFnByName(vm, \"" + fname + "\")")) +
                   ", \"" + fname + "\", da)); }");
        } else throw std::runtime_error("AOT: defer 只支持函数调用");
        g.resetTmp();
        return;
    }

    // try / catch / finally
    // finally 不用 RAII：VM 的语义是「finally 里抛的错会盖掉正在传播的异常」，
    // 而 C++ 析构器在栈展开期抛出会 terminate。改成显式代码后，异常路径下
    // finally 先跑、它的错直接替换原异常，与 VM 完全一致。
    if (auto* t = dynamic_cast<TryStmt*>(s)) {
        if (!t->hasFinally) {
            if (!t->hasCatch) throw std::runtime_error("AOT: try 后面需要 catch 或 finally");
            g.line("{");
            g.ind += "    ";
            g.line("try {");
            g.ind += "    "; g.push(); genStmts(g, t->body); g.pop();
            g.dedent();
            g.line("}");                              // 关闭 try body
            genCatchClauses(g, t, false);
            g.dedent();
            g.line("}");
            g.resetTmp();
            return;
        }
        g.line("{");                                  // 外层块
        g.ind += "    ";
        g.line("try {");
        g.ind += "    ";
        if (t->hasCatch) {
            g.line("try {");
            g.ind += "    "; g.push(); genStmts(g, t->body); g.pop();
            g.dedent();
            g.line("}");
            genCatchClauses(g, t, true);
        } else {
            g.push(); genStmts(g, t->body); g.pop();
        }
        g.dedent();
        g.line("} catch (...) {");                    // 异常路径：finally 先跑
        g.ind += "    ";
        genBlockScoped(g, t->finallyBody);
        g.resetTmp();
        g.line("throw;");                             // finally 没抛就重抛原异常
        g.dedent();
        g.line("}");
        genBlockScoped(g, t->finallyBody);            // 正常路径
        g.resetTmp();
        g.dedent();
        g.line("}");
        g.resetTmp();
        return;
    }

    if (auto* f = dynamic_cast<FuncDef*>(s)) {
        if (!g.inFunction) return;                 // 顶层函数由 emitProgram 单独发射
        aeast::Lambda L;
        L.name = f->name; L.params = std::move(f->params); L.body = std::move(f->body);
        int v = genLambda(g, &L);
        int slot = g.declare(f->name);
        g.line(g.ref(slot) + " = " + g.ref(v) + ";");
        g.resetTmp();
        return;
    }
    throw std::runtime_error("AOT: 未支持的语句");
}

inline int& lambdaCounter() { static int n = 0; return n; }

// 生成一个"函数体"（顶层函数与嵌套 lambda 共用）
inline std::string emitFnBody(CppGen& g, const std::string& fname,
                              const std::vector<aeast::Param>& params,
                              const std::vector<aeast::StmtP>& body) {
    // ① 装箱分析 → ② 预扫槽位 → ③ 生成
    CppGen bc; bc.globals = g.globals; bc.nativeMods = g.nativeMods; bc.nativeFuncs = g.nativeFuncs; bc.funcNames = g.funcNames; bc.curMod = g.curMod;
    computeBoxed(bc, body, params);
    CppGen probe; probe.push(); probe.boxedNames = bc.boxedNames;
    for (const aeast::Param& p : params) probe.declare(p.name);
    prescanStmts(probe, body);
    int nLocal = probe.slotTop, nBox = probe.nBox;

    CppGen fg;
    fg.globals = g.globals; fg.nativeMods = g.nativeMods; fg.nativeFuncs = g.nativeFuncs; fg.funcNames = g.funcNames; fg.curMod = g.curMod;
    fg.upNames = g.upNames; fg.upIdx = g.upIdx;      // 继承本函数的上值表
    fg.boxedNames = bc.boxedNames;
    fg.inFunction = true; fg.fnName = fname; fg.srcFile = g.srcFile;
    fg.maxSlot = nLocal + 12; fg.tmpBase = nLocal; fg.tmpTop = nLocal; fg.slotTop = 0;
    fg.push();

    int nParam = (int)params.size();
    for (int i = 0; i < nParam; i++) {
        int slot = fg.declare(params[i].name);
        if (params[i].variadic) {
            fg.line("{ Value vt = Value(vm->allocTable());");
            fg.line("  for (int j = " + std::to_string(i) + "; j < argc; j++) vt.tab->set(Value((int64_t)(j - " + std::to_string(i) + " + 1)), argv[j]);");
            fg.line("  " + fg.ref(slot) + " = vt; }");
        } else {
            fg.line(fg.ref(slot) + " = (argc > " + std::to_string(i) + ") ? argv[" + std::to_string(i) + "] : Value();");
        }
    }
    for (int i = 0; i < nParam; i++) {
        if (params[i].variadic || !params[i].def) continue;
        int slot = -1; fg.lookup(params[i].name, slot);
        fg.line("if (argc <= " + std::to_string(i) + ") {");
        fg.ind += "    ";
        int v = genExpr(fg, params[i].def.get());
        fg.line(fg.ref(slot) + " = " + fg.ref(v) + ";");
        fg.ind.resize(fg.ind.size() - 4);
        fg.line("}");
    }
    genStmts(fg, body);
    fg.pop();
    // 推导变量等是生成期才 declare 的，真实盒子数以生成后为准
    nBox = fg.nBox > nBox ? fg.nBox : nBox;
    int N = fg.maxSlot + 12;
    std::string code;
    code += "static int " + fname + "(VM* vm, int argc, Value* argv, Value* rv, Upvalue** UP) {\n";
    code += "    (void)UP;\n";
    code += "    Value RV[AE_MAX_RET];\n";
    if (nBox > 0) {
        code += "    Upvalue* BX[" + std::to_string(nBox) + "];\n";
        code += "    for (int i = 0; i < " + std::to_string(nBox) + "; i++) BX[i] = vm->allocBox();\n";
    }
    code += "    Value S[" + std::to_string(N) + "];\n";
    code += "    for (int i = 0; i < " + std::to_string(N) + "; i++) S[i] = Value();\n";
    if (nBox > 0)
        code += "    AeShadowFrame _f(vm, S, " + std::to_string(N) + ", BX, " + std::to_string(nBox) + ");\n";
    else
        code += "    AeShadowFrame _f(vm, S, " + std::to_string(N) + ");\n";
    code += "    AeDeferScope _dfs(vm); AeDsReg _dfsreg(&_dfs);\n";
    { std::string _aen = fname;
      if (_aen.rfind("ae_f_", 0) == 0) _aen = _aen.substr(5);
      code += "    AeFrameLink _fl(vm, \"" + _aen + "\", \"" + fg.srcFile + "\", " +
              std::to_string(fg.fnLine) + ");\n"; }
    for (auto& ln : splitLines(fg.out)) code += "    " + ln + "\n";
    code += "    return 0;\n}\n";
    return fg.nestedCode + code;
}

// Lambda → 闭包值（返回存放它的槽）
inline int genLambda(CppGen& g, aeast::Lambda* L) {
    std::string fname = "ae_L" + std::to_string(++lambdaCounter());
    std::set<std::string> vis;
    for (auto& sc : g.scopes) for (auto& kv : sc) vis.insert(kv.first);
    for (auto& n : g.upNames) vis.insert(n);        // 上值 = 外层的局部变量
    auto fv = freeVarsOf(L->body, L->params, g, &vis);
    std::vector<std::string> ups;
    for (auto& n : fv) {
        // 自引用闭包 func fact(x){...fact...}：此刻 fact 还没被 local 声明，
        // 先在当前层占一个盒子，稍后 local fact = <lambda> 会复用同一个
        if (!g.boxByName.count(n) && !g.upIdx.count(n) && n == L->name) g.declare(n);
        if (g.boxByName.count(n) || g.upIdx.count(n)) ups.push_back(n);
    }
    CppGen lg;
    lg.globals = g.globals; lg.nativeMods = g.nativeMods; lg.nativeFuncs = g.nativeFuncs; lg.funcNames = g.funcNames; lg.curMod = g.curMod;
    lg.upNames = ups;
    for (size_t i = 0; i < ups.size(); i++) lg.upIdx[ups[i]] = (int)i;
    g.nestedCode += emitFnBody(lg, fname, L->params, L->body);

    int t = g.allocTmp();
    g.line("{ Closure* c = vm->allocClosure();");
    g.line("  c->aotFn = " + fname + ";");
    for (auto& n : ups) {
        auto bi = g.boxByName.find(n);
        std::string src = (bi != g.boxByName.end())
            ? ("BX[" + std::to_string(bi->second) + "]")
            : ("UP[" + std::to_string(g.upIdx.at(n)) + "]");
        g.line("  c->ups.push_back(" + src + ");");
    }
    g.line("  " + g.ref(t) + " = Value::makeFunc(c); }");
    return t;
}

inline void emitFunction(CppGen& g, aeast::FuncDef* f) {
    g.fnLine = f->line;
    CppGen top; top.globals = g.globals; top.nativeMods = g.nativeMods; top.nativeFuncs = g.nativeFuncs; top.funcNames = g.funcNames; top.curMod = g.curMod; top.srcFile = g.srcFile;
    g.out += emitFnBody(top, "ae_f_" + f->name, f->params, f->body);
}

inline bool isAeModuleFile(const std::string& name) {
    std::string mp; return findModulePath(name, std::string(), mp);
}
inline bool isAeModuleFileOld(const std::string& name) {
    std::vector<std::string> dirs;
    if (const char* env = getenv("AE_LIB_PATH")) if (*env) dirs.push_back(env);
    std::string sd = aehost::dirOf(std::string());
    dirs.push_back("lib"); dirs.push_back(".");
    for (const std::string& d : dirs) {
        std::string real;
        if (aepath::resolvePath(d + "/" + name + ".m", &real)) return true;
    }
    return false;
}

inline void emitProgram(const aeast::Program& p, const std::string& srcName, std::string& out) {
    using namespace aeast;
    CppGen g; g.srcFile = srcName;
    modTable().clear(); modLoadOrder().clear(); nativeLibSet().clear();
    for (const StmtP& st : p.stmts)                     // 先收集 .m 模块（递归）
        if (auto* im = dynamic_cast<ImpStmt*>(st.get()))
            { std::set<std::string> stk; collectModule(im->modName, srcName, stk); }
    for (const StmtP& st : p.stmts)
        if (auto* im = dynamic_cast<ImpStmt*>(st.get())) {
            std::string mp;
            if (!aecpp::findModulePath(im->modName, srcName, mp)) nativeLibSet().insert(im->modName);
        }
    g.nativeMods = nativeLibSet();          // 含模块内部递归 imp 的库
    {   // 无限定名调用原生函数（imp io 之后 readFile(...) 也能用）
        for (auto& nm : nativeLibSet()) {
            aebuiltin::Module bm;
            int cnt = 0;
            if (aebuiltin::find(nm, &bm)) {
                const AeNativeDef* defs = bm.getExports(&cnt);
                for (int i = 0; i < cnt; i++)
                    if (!g.nativeFuncs.count(defs[i].name))
                        g.nativeFuncs[defs[i].name] = nm + "." + defs[i].name;
            }
        }
    }
    for (const StmtP& st : p.stmts)
        if (auto* f = dynamic_cast<FuncDef*>(st.get())) g.funcNames.insert(f->name);
    {   // 所有出现过的 local / 赋值目标都登记为全局槽（默认 null）
        // 这样「块结束后读到块内 local」= null，与 VM 的行为一致
        std::function<void(const std::vector<StmtP>&)> scanAll =
            [&](const std::vector<StmtP>& v) {
            for (const StmtP& st : v) {
                if (auto* l = dynamic_cast<LocalDecl*>(st.get()))
                    for (auto& n : l->names) if (!g.globals.count(n)) g.globals[n] = (int)g.globals.size();
                if (auto* a = dynamic_cast<Assign*>(st.get()))
                    for (auto& t : a->targets)
                        if (auto* id = dynamic_cast<Ident*>(t.get()))
                            if (!g.globals.count(id->name)) g.globals[id->name] = (int)g.globals.size();
                if (auto* i = dynamic_cast<IfStmt*>(st.get())) {
                    scanAll(i->primary.body);
                    for (auto& b : i->elifs) scanAll(b.body);
                    if (i->hasElse) scanAll(i->elseBody);
                }
                if (auto* w = dynamic_cast<WhileStmt*>(st.get())) scanAll(w->body);
                if (auto* w = dynamic_cast<DoWhileStmt*>(st.get())) scanAll(w->body);
                if (auto* f = dynamic_cast<ForNumStmt*>(st.get())) scanAll(f->body);
                if (auto* f = dynamic_cast<ForInStmt*>(st.get())) scanAll(f->body);
                if (auto* f = dynamic_cast<FuncDef*>(st.get()))   scanAll(f->body);
                if (auto* bk = dynamic_cast<Block*>(st.get())) scanAll(bk->stmts);
                if (auto* t = dynamic_cast<TryStmt*>(st.get())) { scanAll(t->body); scanAll(t->catchBody); scanAll(t->finallyBody); }
            }
        };
        scanAll(p.stmts);
    }

    out = cppPreamble();
    out += "//   源文件: " + srcName + "\n\n";
    int nG = (int)g.globals.size();
    if (nG == 0) nG = 1;
    out += "static Value G[" + std::to_string(nG) + "];\n\n";
    for (auto& nm : modLoadOrder()) {                          // 模块变量数组（先声明）
        int nv = (int)modTable()[nm].varIdx.size();
        if (nv > 0) out += "static Value MODV_" + nm + "[" + std::to_string(nv) + "];\n";
    }
    out += "\n";
    for (const StmtP& st : p.stmts)
        if (auto* f = dynamic_cast<FuncDef*>(st.get()))
            out += "static int ae_f_" + f->name + "(VM* vm, int argc, Value* argv, Value* rv, Upvalue** UP);\n";
    for (auto& nm : modLoadOrder())                             // .m 模块函数
        for (auto& fn : modTable()[nm].funcs)
            out += "static int ae_m_" + nm + "_" + fn +
                   "(VM* vm, int argc, Value* argv, Value* rv, Upvalue** UP);\n";

    std::string funcCode;
    for (const StmtP& st : p.stmts)
        if (auto* f = dynamic_cast<FuncDef*>(st.get())) {
            CppGen fg; fg.globals = g.globals; fg.nativeMods = g.nativeMods; fg.nativeFuncs = g.nativeFuncs; fg.funcNames = g.funcNames; fg.curMod = g.curMod; fg.srcFile = g.srcFile;
            emitFunction(fg, f);
            funcCode += fg.out;
        }
    out += "static Value aeFnByName(VM* vm, const std::string& n);\n";
    out += "static int aeBuiltinId(const std::string& n) {\n";
    for (auto& kv : aecpp::cppBuiltinNames())
        out += "    if (n == \"" + kv.first + "\") return " + std::to_string(kv.second) + ";\n";
    out += "    return -1;\n}\n\n";   // defer 需要按名字取顶层函数
    out += funcCode;

    // 顶层函数名 → 函数值（defer 需要把函数当值登记）
    out += "static Value aeFnByName(VM* vm, const std::string& n) {\n";
    for (auto& nm : modLoadOrder())                       // 模块函数也能当值取
        for (auto& fn : modTable()[nm].funcs) {
            out += "    if (n == \"" + nm + "." + fn + "\") { Closure* c = vm->allocClosure();";
            out += " c->aotFn = ae_m_" + nm + "_" + fn + "; return Value::makeFunc(c); }\n";
        }
    for (const StmtP& st : p.stmts)
        if (auto* f = dynamic_cast<FuncDef*>(st.get())) {
            out += "    if (n == \"" + f->name + "\") { Closure* c = vm->allocClosure();";
            out += " c->aotFn = ae_f_" + f->name + "; return Value::makeFunc(c); }\n";
        }
    out += "    return Value();\n}\n\n";

    CppGen sg; sg.globals = g.globals; sg.nativeMods = g.nativeMods; sg.nativeFuncs = g.nativeFuncs; sg.funcNames = g.funcNames; sg.curMod = g.curMod; sg.srcFile = srcName;
    int nBoxScript = 0;
    {
        // 脚本主体也是一个帧（__script）：顶层 local 被闭包捕获时同样要装箱
        CppGen bc; bc.globals = g.globals; bc.nativeMods = g.nativeMods; bc.nativeFuncs = g.nativeFuncs; bc.funcNames = g.funcNames; bc.curMod = g.curMod;
        computeBoxed(bc, p.stmts);
        sg.boxedNames = bc.boxedNames;
        CppGen probe; probe.push(); probe.boxedNames = bc.boxedNames;
        for (const StmtP& st : p.stmts)
            if (!dynamic_cast<FuncDef*>(st.get()) && !dynamic_cast<ImpStmt*>(st.get()))
            prescan(probe, const_cast<Stmt*>(st.get()));
        sg.maxSlot = probe.slotTop + 12; sg.tmpBase = probe.slotTop; sg.tmpTop = probe.slotTop; sg.slotTop = 0;
        nBoxScript = probe.nBox;
    }
    sg.push();
    for (const StmtP& st : p.stmts)
        if (!dynamic_cast<FuncDef*>(st.get()) && !dynamic_cast<ImpStmt*>(st.get()))
        genStmt(sg, const_cast<Stmt*>(st.get()));
    sg.pop();

    int N = sg.maxSlot + 12;
    // 推导的循环变量也在生成期才 declare，所以真实盒子数以生成后的为准
    nBoxScript = sg.nBox > nBoxScript ? sg.nBox : nBoxScript;
    // .m 模块：变量数组 + 函数 + init（都在 main 之前）
    for (auto& nm : modLoadOrder()) {
        ModInfo& mi = modTable()[nm];
        int nv = (int)mi.varIdx.size();
        for (auto& st : mi.prog->stmts) {
            auto* f = dynamic_cast<FuncDef*>(st.get());
            if (!f) continue;
            CppGen fg; fg.globals = g.globals; fg.nativeMods = g.nativeMods; fg.nativeFuncs = g.nativeFuncs;
            fg.srcFile = nm + ".m"; fg.curMod = nm;
            for (auto& fn : mi.funcs) fg.funcNames.insert(fn);
            out += emitFnBody(fg, "ae_m_" + nm + "_" + f->name, f->params, f->body);
            out += "\n";
        }
        if (nv == 0) continue;
        out += "static void ae_minit_" + nm + "(VM* vm) {\n";
        out += "    Value RV[AE_MAX_RET]; Upvalue** UP = nullptr; Upvalue* BX[1]; BX[0] = vm->allocBox();\n";
        out += "    Value S[128]; for (int i = 0; i < 128; i++) S[i] = Value();\n";
        CppGen ig; ig.globals = g.globals; ig.nativeMods = g.nativeMods; ig.nativeFuncs = g.nativeFuncs;
        ig.srcFile = nm + ".m"; ig.curMod = nm; ig.modInitOf = nm; ig.ind = "    ";
        for (auto& st : mi.prog->stmts)
            if (dynamic_cast<LocalDecl*>(st.get()) || dynamic_cast<Assign*>(st.get()))
                genStmt(ig, const_cast<Stmt*>(st.get()));
        out += "    { AeShadowFrame _f(vm, S, 128, BX, 1);\n";
        out += ig.out;
        out += "    }\n}\n";
    }
    out += "\n";
    out += sg.nestedCode;          // 脚本层定义的 lambda（main 之前输出）
    out += "int main(int argc, char** argv) {\n";
    out += "    (void)argc; (void)argv;\n";
    out += "    aecon::install();\n";
    out += "    VM _vm;\n    VM* vm = &_vm;\n";
    out += "    Value RV[AE_MAX_RET];\n";
    out += "    Upvalue** UP = nullptr;\n";
    out += "    vm->shadowPush(G, " + std::to_string(nG) + ");\n";
    if (nBoxScript > 0) {
        out += "    Upvalue* BX[" + std::to_string(nBoxScript) + "];\n";
        out += "    for (int i = 0; i < " + std::to_string(nBoxScript) + "; i++) BX[i] = vm->allocBox();\n";
    }
    {   // imp 的原生库：运行时注册（内置表 + 动态库回退，与 VM 同一套）
        std::vector<std::string> libs(g.nativeMods.begin(), g.nativeMods.end());
        if (!libs.empty()) {
            out += "    { std::vector<std::string> _libs;\n";
            for (auto& nm : libs) out += "      _libs.push_back(\"" + nm + "\");\n";
            out += "      vm->loadNativeModules(_libs); }\n";
        }
    }
    for (auto& nm : modLoadOrder()) {
        int nv = (int)modTable()[nm].varIdx.size();
        if (nv == 0) continue;
        out += "    vm->shadowPush(MODV_" + nm + ", " + std::to_string(nv) + ");\n";
        out += "    ae_minit_" + nm + "(vm);\n";
    }
    out += "    Value S[" + std::to_string(N) + "];\n";
    out += "    for (int i = 0; i < " + std::to_string(N) + "; i++) S[i] = Value();\n";
    // 顶层兜底：未捕获的错误按 VM 的方式打印（含帧链），再退出
    out += "    try {\n";
    out += "    {\n";
    out += "        AeDeferScope _dfs(vm); AeDsReg _dfsreg(&_dfs);\n";
    if (nBoxScript > 0)
        out += "        AeShadowFrame _f(vm, S, " + std::to_string(N) + ", BX, " + std::to_string(nBoxScript) + ");\n";
    else
        out += "        AeShadowFrame _f(vm, S, " + std::to_string(N) + ");\n";
    for (auto& ln : splitLines(sg.out)) out += "        " + ln + "\n";
    out += "    }\n";
    if (g.funcNames.count("main")) {
        out += "    for (int i = 0; i < AE_MAX_RET; i++) RV[i] = Value();\n";
        out += "    ae_f_main(vm, 0, nullptr, RV, nullptr);\n";
    }
    out += "    } catch (const ExitException& ex) { return ex.code; }\n";
    out += "      catch (const AeErrPos& _p) {\n";
    out += "        fprintf(stderr, \"[-] error: %s\\n\", _p.msg.c_str());\n";
    out += "        return 1;\n";
    out += "    } catch (const std::exception& _e) {\n";
    out += "        fprintf(stderr, \"[-] error: %s\\n\", _e.what());\n";
    out += "        return 1;\n";
    out += "    }\n";
    out += "    return 0;\n}\n";
}

} // namespace aecpp
#endif
