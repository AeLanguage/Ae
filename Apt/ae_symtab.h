#pragma once
// 符号表（词法作用域 / 上值解析 / 函数签名）
#include <string>
#include <vector>
#include <map>
#include <set>
#include <cstdint>
#include <stdexcept>
#include "ae_common.h"

// 符号表
// 1.7 闭包：上值描述（必须与 ae.cpp 里的 UpvalDesc 一致）
// fromParentLocal=true → 抓父帧的 locals[index]（引用捕获，共享变量）
// fromParentLocal=false → 继承父闭包的第 index 个上值（多层嵌套的中转）
struct UpvalDesc {
    bool     fromParentLocal;
    uint16_t index;
};

struct VarInfo {
    enum Kind { GLOBAL, LOCAL, UPVAL } kind;   // UPVAL = 闭包捕获的外部变量
    uint16_t slot;
};

class SymbolTable {
public:
    map<string, uint16_t> globals;
    uint16_t globalCount = 0;

    // 词法作用域栈（P0-1 修复）
    // 旧实现用一张扁平的 name→slot 表：allocLocal 对同名变量永远返回同一个槽位，
    // 于是 `local x = 1; if (true) { local x = 99 }` 里的内层 local 直接改写了
    // 外层变量（实测 prln(x) 得 99），块外还能读到块内声明的变量。
    //
    // 现在：scopes[0] 是函数作用域，每个受控块进出时 push/pop；
    // · 同名变量在不同作用域分到不同槽位→ 真正的遮蔽
    // · 出作用域时水位回退 → 槽位可复用，帧大小只取决于嵌套深度
    // · 作用域外不可见 → local 不再泄漏
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

    // 默认参数：funcId → 每个参数位的"加载常量"字节码片段。
    // 片段为空 = 该参数是必传的；非空 = 可选（调用时缺了就用它补）。
    // 按参数位置存，长度与 funcParamCounts 一致。
    map<uint16_t, vector<vector<uint8_t>>> funcDefaults;
    // funcId → 必传参数个数（前面没有默认值的那几个）
    map<uint16_t, uint16_t> funcMinArgs;
    // funcId → 每个参数位默认值的常量池下标，写进 FUNC_DEF 头供 VM 动态调用补参。
    // 0xFFFF = 必传；0xFFFE = true；0xFFFD = false（常量池里没有 bool 类型）
    map<uint16_t, vector<uint16_t>> funcDefaultConsts;
    // 变参：funcId → 变参名的参数槽位（一定是最末位）；不在表里 = 没有变参
    map<uint16_t, uint16_t> funcVariadicSlot;

    bool inFunction = false;

    // 1.7 闭包：当前函数的上值表 + 外层函数上下文栈
    // 编译嵌套函数时，把当前上下文搬进 ctxStack，换成新函数的；退出时恢复。
    // 访问外层变量时，resolveAux 递归向外找，并在回程逐层登记上值 —— 这正是
    // Lua 的 singlevaraux 做法：最近的那层登记"抓父帧局部"，更外层登记"继承父上值"。
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

    // 本函数体发射过的 CLOSURE 条数（用于判断 CLOSE_SLOT 是否可以省略）
    int          closureCount = 0;
    void beginFunction() {
        scopes.clear();
        localCount = 0;
        maxLocalCount = 0;
        curUpvals.clear();           // 顶层函数没有上值
        curUpvalNames.clear();
        closureCount = 0;            // 每个函数体独立统计
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
        // 1.7：再看外层函数（闭包捕获）——会沿途登记上值，所以 resolve 不再是 const
        if (!ctxStack.empty()) {
            uint16_t us = 0;
            if (resolveAux(0, name, &us) == RES_UPVAL) return {VarInfo::UPVAL, us};
        }
        // 模块内：裸名先看是不是本模块的模块变量（key 带 "模块名." 前缀）
        if (!modPrefix.empty()) {
            auto mit = globals.find(modPrefix + name);
            if (mit != globals.end()) return {VarInfo::GLOBAL, mit->second};
        }
        auto it = globals.find(name);
        if (it != globals.end()) return {VarInfo::GLOBAL, it->second};
        return {VarInfo::GLOBAL, (uint16_t)-1};
    }

    // 模块变量：编译 .m 模块时 = "模块名."，让模块里的裸名能解析到自己的模块变量。
    // 存进 globals 的 key 是「前缀+名字」，所以两个模块各有一个 pi 也不会撞槽；
    // 主文件（前缀为空）查不到它们，只能走 math.pi 这种限定访问。
    string modPrefix;

    bool forbidNewGlobals = false;      // 编译 .m 模块时置位：模块不许有隐式全局
    uint16_t allocGlobal(const string& name) {
        auto it = globals.find(name);
        if (it != globals.end()) return it->second;
        if (forbidNewGlobals)
            throw runtime_error("模块内不允许隐式全局变量（请写成 local " + name + " = ...）");
        uint16_t slot = globalCount++;
        globals[name] = slot;
        return slot;
    }
    // 给模块变量分配全局槽（带模块前缀，且不受 forbidNewGlobals 限制）
    uint16_t allocModuleVar(const string& name) {
        string key = modPrefix + name;
        auto it = globals.find(key);
        if (it != globals.end()) return it->second;
        uint16_t slot = globalCount++;
        globals[key] = slot;
        return slot;
    }
    // 模块变量的槽（未登记返回 -1）
    uint16_t findModuleVar(const string& name) const {
        auto it = globals.find(modPrefix + name);
        return it == globals.end() ? (uint16_t)-1 : it->second;
    }
    // 只在当前作用域内查重：嵌套块里的同名 local 会拿到新槽位（遮蔽）
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
