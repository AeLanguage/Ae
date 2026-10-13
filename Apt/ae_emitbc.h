#pragma once
// Ae 字节码后端：AST → .aeo
// 编译流程：Lexer → Parser → AST → 本文件 → .aeo 字节码。
// 前向引用天然成立：AST 是完整的一棵树，"调用后面定义的函数"不需要预扫描。
#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <stdexcept>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <unistd.h>
#include "ae_common.h"
#include "ae_lexer.h"
#include "ae_ast.h"
#include "ae_parser.h"
#include "ae_symtab.h"
#include "ae_constpool.h"
#include "ae_libhost.h"
#include "ae_builtin.h"
#include "ae_path.h"

namespace aeemit {

using namespace aeast;

// 操作码（须与 VM / aed 一致）
namespace OP {
    constexpr uint8_t LOAD_CONST  = 0x01;
    constexpr uint8_t CALL        = 0x02;   // 内置：u8 bid, u8 argc, u8 want
    constexpr uint8_t HALT        = 0x03;
    constexpr uint8_t STORE_G     = 0x04;
    constexpr uint8_t LOAD_G      = 0x05;
    constexpr uint8_t IADD=0x06, ISUB=0x07, IMUL=0x08, IDIV=0x09;
    constexpr uint8_t ADD =0x0A, SUB =0x0B, MUL =0x0C, DIV =0x0D;
    constexpr uint8_t ITOD=0x0E, IMOD=0x0F;
    constexpr uint8_t CMP_EQ=0x10, CMP_NEQ=0x11, CMP_LT=0x12, CMP_LE=0x13;
    constexpr uint8_t CMP_GT=0x14, CMP_GE=0x15;
    constexpr uint8_t TRUE_=0x1C, FALSE_=0x1D, NOT_=0x1E;
    constexpr uint8_t BNOT=0x1F, BAND=0x20, BOR=0x21, BXOR=0x22;
    constexpr uint8_t SHL =0x23, SHR =0x24, POW =0x25;
    constexpr uint8_t NATIVE=0x26;          // u16 const, u8 argc, u8 want
    constexpr uint8_t IN   =0x29;
    constexpr uint8_t JZ   =0x2A;           // 相对偏移 int16
    constexpr uint8_t JMP  =0x2B;
    constexpr uint8_t SLICE=0x2C;
    constexpr uint8_t DEFER=0x2D, DEFER_NATIVE=0x2E, END_DEFER=0x2F;
    constexpr uint8_t STORE_L=0x30, LOAD_L=0x31;
    constexpr uint8_t FUNC_DEF=0x40;
    constexpr uint8_t CALL_FUNC=0x41;       // u16 fid, u8 argc, u8 want
    constexpr uint8_t RET=0x42;             // u8 n
    constexpr uint8_t POP=0x43;
    constexpr uint8_t CLOSURE=0x44;         // u16 fid, u8 nups
    constexpr uint8_t GET_UPVAL=0x45, SET_UPVAL=0x46;
    constexpr uint8_t CLOSE_SLOT=0x47;
    constexpr uint8_t CALL_VALUE=0x48;      // u8 argc, u8 want
    constexpr uint8_t NEW_TABLE=0x50;
    constexpr uint8_t TABLE_GET=0x51, TABLE_SET=0x52;
    constexpr uint8_t TABLE_KEYS=0x53, TABLE_LEN=0x54;
}

// 编译期推断的结果类型（决定发整数指令还是通用指令）
enum class RT { Null, Int, Double, Str, Bool, Table, Func };

class Emitter {
public:
    // 产物
    ConstantPool cp;
    SymbolTable  syms;
    CodeBuf      output;
    CodeBuf*     currentCode = &output;

    struct FuncBlob {
        std::vector<UpvalDesc> upvals;
        std::vector<uint8_t>   code;
        std::vector<CodeBuf::LineEntry> lines;
        std::string  name;
        uint16_t funcId = 0, paramCount = 0, localCount = 0;
        std::vector<uint16_t> defaultConsts;
        uint16_t variadicSlot = 0xFFFF;
    };
    std::vector<FuncBlob> funcDefs;
    std::vector<std::pair<uint16_t,std::string>> funcNames;
    std::vector<CodeBuf::LineEntry> allLines;
    std::string srcName = "<source>";
public:
    std::vector<std::string> fileNames;
    std::vector<std::string> importedModules;
    std::vector<aehost::LoadedModule> loadedModules;

    // 模块与库
    struct ModFunc {
        uint16_t funcId = 0, params = 0, minParams = 0;
        std::vector<std::vector<uint8_t>> defaults;
        uint16_t variadicSlot = 0xFFFF;
    };
    struct NativeInfo { std::string module, name; int minArgs=0, maxArgs=0; };
    std::map<std::string, NativeInfo> natives;
    std::map<std::string, std::map<std::string, ModFunc>> modules;
    std::map<std::string, ModFunc>                       impFuncs;
    std::map<std::string, std::map<std::string, uint16_t>> moduleVars;
    std::map<std::string, uint16_t> moduleInitId;
    std::vector<std::string> moduleLoadOrder;
    std::set<std::string> importedSet, moduleFiles;

    uint16_t nextUserFuncId = 8;
    std::string curModule;
    int  handlerDepth = 0;
    int  finGuard = 0;   // >0 = 处在带 finally 的 try 体内（return/break 不得跨越）
    bool inLoop = false;

    explicit Emitter(const std::string& path) : srcName(path) { fileNames.push_back(path); }

    // 入口
    void collectAssigned(std::vector<StmtP>& v) {
        for (StmtP& st : v) {
            if (auto* a = dynamic_cast<Assign*>(st.get()))
                for (auto& t : a->targets)
                    if (auto* id = dynamic_cast<Ident*>(t.get())) assignedNames.insert(id->name);
            if (auto* l = dynamic_cast<LocalDecl*>(st.get()))
                for (auto& n : l->names) assignedNames.insert(n);
            if (auto* f = dynamic_cast<FuncDef*>(st.get())) collectAssigned(f->body);
            if (auto* b = dynamic_cast<Block*>(st.get()))  collectAssigned(b->stmts);
        }
    }
    void emit(Program& p) {
        // ① 处理 imp（可能递归编译 .m 模块）
        for (StmtP& s : p.stmts) {
            auto* im = dynamic_cast<ImpStmt*>(s.get());
            if (im) importLibrary(im->modName);
        }
        collectAssigned(p.stmts);
        // ② 顶层：函数定义先登记（支持前向引用），再发射
        for (StmtP& s : p.stmts)
            if (auto* f = dynamic_cast<FuncDef*>(s.get())) declareFunc(f);
        // ③ 脚本主体（ 也是一个独立帧：包成 <script> FuncBlob，否则 STORE_L 无帧可用）
        syms.inFunction = true;
        syms.beginFunction();
        scriptFuncId = nextUserFuncId++;
        syms.functions["__script"] = scriptFuncId;
        CodeBuf scriptBuf; CodeBuf* codeSaved = currentCode;
        currentCode = &scriptBuf;
        scriptBuf.markLine(1);
        // 模块初始化放在脚本主体最前（被 imp 的模块先完成，顺序天然满足依赖）
        for (const std::string& m : moduleLoadOrder) {
            auto mi = moduleInitId.find(m);
            if (mi == moduleInitId.end()) continue;
            currentCode->emit(OP::CALL_FUNC);
            currentCode->u16(mi->second); currentCode->u8(0); currentCode->u8(0);
        }
        // 模块初始化之后，把落账位置切到main 所在行（没有 main 就用第一条真实语句）。
        // 注意 markLine 只推一条记录、不更新 pendingLine，后续 emit() 仍会用旧的
        // pendingLine —— 所以必须连 pending 一起设，否则 <script> 帧永远显示 1:1。
        {
            int sl = 0;
            for (StmtP& st : p.stmts) {
                if (auto* fd = dynamic_cast<FuncDef*>(st.get()))
                    if (fd->name == "main") { sl = fd->line; break; }
            }
            if (!sl) for (StmtP& st : p.stmts) {
                if (dynamic_cast<ImpStmt*>(st.get())) continue;
                sl = st->line; break;
            }
            if (sl) { scriptBuf.pendingLine = (uint32_t)sl; scriptBuf.pendingCol = 1;
                      scriptBuf.markPos((uint32_t)sl, 1); }
        }
        for (StmtP& s : p.stmts) {
            if (dynamic_cast<FuncDef*>(s.get()) || dynamic_cast<ImpStmt*>(s.get())) continue;
            stmt(s.get());
        }
        auto it = syms.functions.find("main");
        if (it != syms.functions.end()) {
            currentCode->emit(OP::CALL_FUNC);
            currentCode->u16(it->second); currentCode->u8(0); currentCode->u8(0);
        }
        scriptBuf.emit(OP::END_DEFER); scriptBuf.u16(0);
        scriptBuf.emit(OP::RET); scriptBuf.u8(0);
        currentCode = codeSaved;
        uint16_t scriptLocals = syms.maxLocalCount;
        syms.inFunction = false;
        syms.endFunction();

        // ④ 函数体发射
        for (StmtP& s : p.stmts)
            if (auto* f = dynamic_cast<FuncDef*>(s.get())) emitFunc(f);

        // ⑤ 入口 stub：CALL __script; HALT
        output.markLine(1);
        output.emit(OP::CALL_FUNC); output.u16(scriptFuncId); output.u8(0); output.u8(0);
        output.emit(OP::HALT);

        // ⑥ FUNC_DEF 段：脚本函数放最前，其后是用户函数
        {
            FuncBlob sb;
            sb.code = scriptBuf.code; sb.lines = scriptBuf.lines;
            sb.name = "<script>"; sb.funcId = scriptFuncId;
            sb.paramCount = 0; sb.localCount = scriptLocals;
            funcDefs.insert(funcDefs.begin(), std::move(sb));
        }
        for (const auto& L : output.lines) allLines.push_back(L);
        for (const FuncBlob& f : funcDefs) emitFuncBlob(f);
        std::sort(allLines.begin(), allLines.end(),
                  [](const CodeBuf::LineEntry& a, const CodeBuf::LineEntry& b){ return a.off < b.off; });
    }

    uint16_t scriptFuncId = 0;

private:
    // 库与模块
    void importLibrary(const std::string& modName) {
        if (importedSet.count(modName)) return;
        aebuiltin::Module bm;
        if (aebuiltin::find(modName, &bm)) {
            int n = 0;
            const AeNativeDef* defs = bm.getExports(&n);
            if (!defs || n <= 0) throw std::runtime_error("内置库 " + modName + " 没有导出任何函数");
            for (int i = 0; i < n; i++) registerNative(modName, defs[i].name, defs[i].minArgs, defs[i].maxArgs);
            importedModules.push_back(modName);
            importedSet.insert(modName);
            return;
        }
        // ② Ae 源码模块（.m）：不用编译，解析后编进本程序
        std::string mpath;
        if (findModuleFile(modName, mpath)) {
            loadModule(modName);
            importedSet.insert(modName);
            return;
        }
        aehost::LoadedModule lm;
        std::vector<std::string> tried;
        std::string hint = srcName == "<source>" ? std::string() : aehost::dirOf(srcName);
        if (aehost::loadLibrary(modName, hint, lm, tried)) {
            if (lm.mod->abiVersion != AE_ABI_VERSION)
                throw std::runtime_error("库 " + modName + " 的 ABI 版本是 " +
                    std::to_string(lm.mod->abiVersion) + "，本工具链支持 " + std::to_string(AE_ABI_VERSION));
            int n = 0;
            const AeNativeDef* defs = aehost::readExports(lm, n);
            if (!defs || n <= 0) throw std::runtime_error("库 " + modName + " 没有导出任何函数");
            for (int i = 0; i < n; i++) registerNative(modName, defs[i].name, defs[i].minArgs, defs[i].maxArgs);
            loadedModules.push_back(lm);
            importedModules.push_back(modName);
            importedSet.insert(modName);
            return;
        }
        throw std::runtime_error(aehost::triedText(modName, tried));
    }
    void registerNative(const std::string& mod, const std::string& name, int mn, int mx) {
        NativeInfo info{mod, name, mn, mx};
        natives[mod + "." + name] = info;
        if (!natives.count(name)) natives[name] = info;
    }

    // 模块（.m）：找文件 → 解析 → 编译成带前缀的函数 + 顶层变量 init
    // 可执行文件所在目录：.m 模块放在 aec/ae 旁边的 lib/ 下
    static std::string exeDir() {
        std::string r;
#ifdef _WIN32
        wchar_t buf[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) r = aepath::dirOf(aepath::toUtf8(std::wstring(buf, n)));
#else
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) { buf[n] = 0; r = aepath::dirOf(std::string(buf)); }
#endif
        return r;
    }
    std::vector<std::string> libSearchDirs() const {
        std::vector<std::string> d;
        const char* env = getenv("AE_LIB_PATH");
        if (env && *env) d.push_back(env);
        std::string ed = exeDir();
        if (!ed.empty()) { d.push_back(ed + "/lib"); d.push_back(ed); }
        std::string sd = aehost::dirOf(srcName);
        if (!sd.empty() && sd != ed) { d.push_back(sd + "/lib"); d.push_back(sd); }
        d.push_back("lib"); d.push_back(".");
        return d;
    }
    bool findModuleFile(const std::string& name, std::string& out) {
        for (const std::string& dir : libSearchDirs()) {
            std::string p = dir + "/" + name + ".m";
            std::string real;
            if (aepath::resolvePath(p, &real)) { out = real; return true; }
        }
        return false;
    }

    void loadModule(const std::string& name) {
        if (modules.count(name) || moduleInitId.count(name)) return;
        for (const std::string& m : moduleStack)
            if (m == name) throw std::runtime_error("循环导入：'" + name + "' 已经在导入链上了");
        std::string path;
        if (!findModuleFile(name, path)) throw std::runtime_error("找不到模块文件 " + name + ".m");
        if (moduleFiles.count(path)) return;
        moduleFiles.insert(path);

        std::string msrc;
        if (!aepath::readFileText(path, &msrc)) throw std::runtime_error("无法打开模块文件: " + path);

        // 保存外层状态
        std::string savedModule = curModule;
        uint16_t    savedFile   = curFileIdx;
        bool        savedForbid = syms.forbidNewGlobals;
        std::string savedPrefix = syms.modPrefix;
        CodeBuf*    savedCode   = currentCode;

        curModule = name;
        curFileIdx = fileIndexFor(path);
        syms.forbidNewGlobals = true;
        syms.modPrefix = name + ".";
        moduleStack.push_back(name);

        aeparse::Parser P(msrc, path, curFileIdx);
        Program mp = P.parseProgram(name);
        for (StmtP& st : mp.stmts)
            if (auto* im = dynamic_cast<ImpStmt*>(st.get())) importLibrary(im->modName);

        // ① 登记函数签名（模块内允许先用后定义）
        for (StmtP& st : mp.stmts)
            if (auto* f = dynamic_cast<FuncDef*>(st.get())) declareFunc(f);
        // ② 顶层变量登记（key = "模块名.变量名"）
        bool modVarSeen = false;
        for (StmtP& st : mp.stmts) {
            if (auto* l = dynamic_cast<LocalDecl*>(st.get())) {
                for (auto& n : l->names) { syms.allocModuleVar(n); modVarSeen = true; }
            } else if (auto* a = dynamic_cast<Assign*>(st.get())) {
                for (auto& t : a->targets)
                    if (auto* id = dynamic_cast<Ident*>(t.get())) { syms.allocModuleVar(id->name); modVarSeen = true; }
            }
        }
        // ③ 函数体
        for (StmtP& st : mp.stmts)
            if (auto* f = dynamic_cast<FuncDef*>(st.get())) emitFunc(f);
        // ④ 顶层变量 → init 函数
        if (modVarSeen) {
            uint16_t initId = nextUserFuncId++;
            syms.inFunction = true; syms.beginFunction();
            CodeBuf ib; currentCode = &ib;
            ib.markLine(1);
            inModuleInit = true;
            for (StmtP& st : mp.stmts) {
                if (auto* l = dynamic_cast<LocalDecl*>(st.get())) { localDecl(l); continue; }
                if (auto* a = dynamic_cast<Assign*>(st.get()))    { assign(a);    continue; }
            }
            ib.emit(OP::RET); ib.u8(0);
            inModuleInit = false;
            currentCode = savedCode;
            uint16_t lc = syms.maxLocalCount;
            syms.inFunction = false; syms.endFunction();
            FuncBlob fb; fb.code = ib.code; fb.lines = ib.lines;
            fb.name = name + ".<init>"; fb.funcId = initId; fb.localCount = lc;
            funcDefs.push_back(std::move(fb));
            moduleInitId[name] = initId;
            // 导出的模块变量（_ 开头 / local 的不导出）
            std::map<std::string, uint16_t> ex;
            for (StmtP& st : mp.stmts) {
                if (auto* a = dynamic_cast<Assign*>(st.get()))
                    for (auto& t : a->targets)
                        if (auto* id = dynamic_cast<Ident*>(t.get()))
                            if (!id->name.empty() && id->name[0] != '_')
                                ex[id->name] = syms.findModuleVar(id->name);
            }
            moduleVars[name] = ex;
        }
        moduleLoadOrder.push_back(name);
        moduleStack.pop_back();
        curModule = savedModule; curFileIdx = savedFile;
        syms.forbidNewGlobals = savedForbid; syms.modPrefix = savedPrefix;
    }

    bool inModuleInit = false;   // 模块顶层变量初始化中（local 也写模块全局槽）
    std::set<std::string> assignedNames;   // 全文件出现过的"被赋值的名字"
    std::vector<std::string> moduleStack;
    uint16_t curFileIdx = 0;
    uint16_t fileIndexFor(const std::string& p) {
        for (size_t i = 0; i < fileNames.size(); i++) if (fileNames[i] == p) return (uint16_t)i;
        fileNames.push_back(p); return (uint16_t)(fileNames.size() - 1);
    }

    // 函数
    void declareFunc(FuncDef* f) {
        std::string key = curModule.empty() ? f->name : (curModule + "." + f->name);
        if (syms.functions.count(key)) throw std::runtime_error("函数 '" + f->name + "' 重复定义");
        if (curModule.empty() && builtinId(f->name) != 0xFF)
            throw std::runtime_error("'" + f->name + "' 是内建函数名，不能用来定义函数");
        uint16_t fid = nextUserFuncId++;
        syms.functions[key] = fid;
        // 参数元信息在声明阶段就算好：脚本主体可能在函数体发射之前就调用它，
        // 那时 funcDefaults 还没填 → 参数个数检查会失效。
        std::vector<uint16_t> dc; uint16_t vs = 0xFFFF;
        for (size_t i = 0; i < f->params.size(); i++) {
            if (f->params[i].variadic) { vs = (uint16_t)i; dc.push_back(0xFFFF); continue; }
            dc.push_back(f->params[i].def ? constIdxOf(f->params[i].def.get()) : 0xFFFF);
        }
        funcDefaults[fid] = dc; funcVariadic[fid] = vs;
        syms.funcParamCounts[fid] = (uint16_t)f->params.size();
    }

    void emitFunc(FuncDef* f) {
        // 写在函数体里面的 func name(...) 是局部闭包（能捕获外层变量），
        // 等价于 local name = func name(...) {...} —— 与旧实现一致。
        // 只有顶层 / 模块顶层声明才是全局静态函数。
        bool nested = !syms.scopes.empty() || !syms.ctxStack.empty();
        uint16_t selfSlot = 0xFFFF;
        if (nested) {
            selfSlot = syms.allocLocal(f->name);
            syms.pushCtx(f->name);
        } else {
            std::string key = curModule.empty() ? f->name : (curModule + "." + f->name);
            if (!syms.functions.count(key)) syms.functions[key] = nextUserFuncId++;
            syms.inFunction = true; syms.beginFunction();
        }
        uint16_t funcId = 0;
        if (nested) funcId = nextUserFuncId++;
        else { std::string k2 = curModule.empty() ? f->name : (curModule + "." + f->name);
               funcId = syms.functions.count(k2) ? syms.functions[k2] : nextUserFuncId++; }
        CodeBuf bodyBuf;
        CodeBuf* saved = currentCode;
        currentCode = &bodyBuf;
        bodyBuf.markLine((uint32_t)f->line);

        std::vector<uint16_t> defaultConsts;
        uint16_t variadicSlot = 0xFFFF;
        for (const Param& pm : f->params) {
            uint16_t slot = syms.allocParam(pm.name);
            if (pm.variadic) { variadicSlot = slot; defaultConsts.push_back(0xFFFF); continue; }
            defaultConsts.push_back(pm.def ? constIdxOf(pm.def.get()) : 0xFFFF);
        }
        syms.funcParamCounts[funcId] = (uint16_t)f->params.size();
        funcDefaults[funcId] = defaultConsts;
        funcVariadic[funcId] = variadicSlot;
        if (!nested) {                        // 顶层函数：按"声明时的 key"再存一份，
            std::string kk = curModule.empty() ? f->name : (curModule + "." + f->name);
            auto dit2 = syms.functions.find(kk);
            if (dit2 != syms.functions.end() && dit2->second != funcId) {
                funcDefaults[dit2->second] = defaultConsts;
                funcVariadic[dit2->second] = variadicSlot;
            }
        }

        for (StmtP& s : f->body) stmt(s.get());

        bodyBuf.emit(OP::END_DEFER); bodyBuf.u16(0);
        bodyBuf.emit(OP::RET); bodyBuf.u8(0);

        currentCode = saved;
        uint16_t frameLocals = syms.maxLocalCount;
        std::vector<UpvalDesc> ups;
        if (nested) { syms.popCtx(&ups); } else { syms.inFunction = false; syms.endFunction(); }

        FuncBlob fb;
        fb.code = bodyBuf.code; fb.lines = bodyBuf.lines; fb.upvals = ups;
        fb.name = f->name; fb.funcId = funcId;
        fb.paramCount = (uint16_t)f->params.size();
        fb.localCount = frameLocals;
        fb.defaultConsts = defaultConsts;
        fb.variadicSlot = variadicSlot;
        if (nested) {
            // 局部闭包：在此处造闭包并存进局部槽，调用点走"变量 → 动态调用"
            funcDefs.push_back(std::move(fb));
            syms.closureCount++;
            currentCode->emit(OP::CLOSURE);
            currentCode->u16(funcId); currentCode->u8((uint8_t)ups.size());
            genStore(selfSlot);
            return;
        }

        if (!curModule.empty() && !f->name.empty() && f->name[0] != '_') {
            ModFunc mf;
            mf.funcId = funcId;
            mf.params = (uint16_t)f->params.size();
            mf.defaults.resize(defaultConsts.size());
            mf.variadicSlot = variadicSlot;
            modules[curModule][f->name] = mf;
            if (!impFuncs.count(f->name)) impFuncs[f->name] = mf;
        }
        funcDefs.push_back(std::move(fb));
    }

    // 默认参数：只接受字面量，直接取常量池下标
    uint16_t constIdxOf(Expr* e) {
        if (auto* n = dynamic_cast<NumLit*>(e))
            return n->isFloat ? cp.addDouble(n->d) : cp.addIntAuto(n->i);
        if (auto* s = dynamic_cast<StrLit*>(e))  return cp.addString(s->s);
        if (auto* b = dynamic_cast<BoolLit*>(e)) return b->v ? 0xFFFE : 0xFFFD;
        if (dynamic_cast<NullLit*>(e))           return cp.addNull();
        throw std::runtime_error("参数默认值必须是字面量");
    }

    void emitFuncBlob(const FuncBlob& f) {
        uint32_t headerSize = 11 + 2 + (uint32_t)(3 * f.upvals.size()) + 2
                            + 2 * (uint32_t)f.defaultConsts.size() + 2;
        uint32_t bodyStart = (uint32_t)output.size() + headerSize;
        output.emit(OP::FUNC_DEF);
        output.u16(f.funcId);
        output.u16(f.paramCount);
        output.u16(f.localCount);
        output.u32((uint32_t)f.code.size());
        output.u16((uint16_t)f.upvals.size());
        for (const UpvalDesc& d : f.upvals) { output.u8(d.fromParentLocal ? 1 : 0); output.u16(d.index); }
        output.u16((uint16_t)f.defaultConsts.size());
        for (uint16_t d : f.defaultConsts) output.u16(d);
        output.u16(f.variadicSlot);
        for (uint8_t b : f.code) output.emit(b);
        for (const auto& L : f.lines) allLines.push_back({L.off + bodyStart, L.line, L.col, L.file});
        funcNames.push_back({f.funcId, f.name});
    }

    // 语句
public:
    // 指令长度表（含操作数）：统计与反汇编都靠它
    static int instrLen(uint8_t op) {
        switch (op) {
            case 0x01: case 0x04: case 0x05:
            case 0x30: case 0x31: case 0x2A: case 0x2B:
            case 0x2D: return 3;
            case 0x2E: return 5;
            case 0x2F: return 3;
            case 0x50: return 3;
            case 0x02: return 4;
            case 0x41: return 5;
            case 0x44: return 4;
            case 0x45: case 0x46: case 0x47: case 0x48: return 3;
            case 0x42: return 2;
            case 0x55: return 3;
            case 0x26: return 5;
            case 0x32: return 5;
            case 0x40: return 1;      // FUNC_DEF 头，统计时单独跳过
            default: return 1;
        }
    }

    // stats：静态指令总数与分布（诊断指令冗余用）
    void dumpStats() const {
        std::map<int,int> cnt;
        std::vector<std::pair<std::string, std::vector<uint8_t>>> regions;
        for (const FuncBlob& f : funcDefs) regions.push_back({f.name, f.code});
        int total = 0;
        for (auto& r : regions) {
            const std::vector<uint8_t>& c = r.second;
            size_t p = 0;
            while (p < c.size()) {
                uint8_t op = c[p++];
                cnt[op]++; total++;
                p += (size_t)(instrLen(op) - 1);
            }
        }
        std::cerr << "[stats] 静态指令总数 " << total << "（" << regions.size() << " 个函数体）\n";
        std::vector<std::pair<int,int>> v(cnt.begin(), cnt.end());
        std::sort(v.begin(), v.end(), [](auto&a, auto&b){ return a.second > b.second; });
        std::cerr << "[stats] 指令分布 TOP15:\n";
        for (size_t i = 0; i < v.size() && i < 15; i++)
            std::cerr << "   0x" << std::hex << std::uppercase << v[i].first << std::dec
                      << "  " << v[i].second << "  " << (v[i].second * 100.0 / total) << "%\n";
    }

    // 运行期错误要指回源码位置：发射前把"当前位置"同步给 CodeBuf
    void syncPos(int line, int col) {
        currentCode->pendingLine = (uint32_t)line;
        currentCode->pendingCol  = (uint32_t)col;
        currentCode->pendingFile = curFileIdx;
    }
    void stmt(Node* n) {
        if (!n) return;
        syncPos(n->line, n->col);
        if (auto* b = dynamic_cast<Block*>(n)) {
            syms.pushScope();
            for (StmtP& s : b->stmts) stmt(s.get());
            endScope(); return;
        }
        if (auto* l = dynamic_cast<LocalDecl*>(n)) { localDecl(l); return; }
        if (auto* a = dynamic_cast<Assign*>(n))    { assign(a);    return; }
        if (auto* c = dynamic_cast<CompoundAssign*>(n)) { compound(c); return; }
        if (auto* i = dynamic_cast<IfStmt*>(n))    { ifStmt(i);    return; }
        if (auto* w = dynamic_cast<WhileStmt*>(n)) { whileStmt(w); return; }
        if (auto* d = dynamic_cast<DoWhileStmt*>(n)) { doWhile(d); return; }
        if (auto* f = dynamic_cast<ForInStmt*>(n)) { forIn(f);     return; }
        if (auto* f = dynamic_cast<ForNumStmt*>(n)){ forNum(f);    return; }
        if (auto* r = dynamic_cast<ReturnStmt*>(n)){ ret(r);       return; }
        if (auto* b = dynamic_cast<BreakStmt*>(n)) {
            if (!inLoop) throw std::runtime_error("break 只能在循环里使用");
            if (finGuard > 0)
                throw std::runtime_error(aePack(curFileIdx, b->line, b->col, 5,
                    "break 不能跳出带 finally 的 try —— 它会跳过 finally 不执行。"
                    "（清理请改用 defer，或把 try/finally 挪到循环外面）"));
            currentCode->emit(OP::JMP);
            size_t pp = placeholderU16();
            if (b->level <= 1) breakPatches.push_back(pp);
            else {
                int target = loopDepth - (b->level - 1);
                if (target < 1)
                    throw std::runtime_error(aePack(curFileIdx, b->line, b->col, 5, "超出当前循环嵌套深度"));
                outerBreaks[target].push_back(pp);
            }
            return;
        }
        if (auto* ct = dynamic_cast<ContinueStmt*>(n)) {
            if (!inLoop) throw std::runtime_error("continue 只能在循环里使用");
            currentCode->emit(OP::JMP);
            size_t pp = placeholderU16();
            if (ct->level <= 1) contPatches.push_back(pp);
            else {
                int target = loopDepth - (ct->level - 1);
                if (target < 1)
                    throw std::runtime_error(aePack(curFileIdx, ct->line, ct->col, 8, "超出当前循环嵌套深度"));
                outerConts[target].push_back(pp);
            }
            return;
        }
        if (auto* e = dynamic_cast<ExprStmt*>(n)) {
            expr(e->e.get());
            currentCode->emit(OP::POP); return;
        }
        if (auto* f = dynamic_cast<FuncDef*>(n)) { emitFunc(f); return; }
        if (auto* d = dynamic_cast<DeferStmt*>(n)) { deferStmt(d); return; }
        if (auto* t = dynamic_cast<TryStmt*>(n))   { tryStmt(t);   return; }
        if (dynamic_cast<ImpStmt*>(n)) return;
        throw std::runtime_error("内部错误：未知的语句类型");
    }

    // defer：注册"作用域退出时要跑的调用"。
    // 用户函数 → DEFER(u16 fid, u8 argc)；内置 → DEFER_BUILTIN；原生 → DEFER_NATIVE
    uint16_t curScopeId = 0;
    uint16_t deferScope = 0;
    uint16_t nextDeferScope = 0;
    std::set<uint16_t> deferScopes;
    void markDefer() { deferScopes.insert(deferScope); }
    bool scopeHasDefer(uint16_t sc) const { return deferScopes.count(sc) != 0; }
    void scopedBlock(std::vector<StmtP>& body) {
        syms.pushScope();
        uint16_t outer = deferScope;
        deferScope = ++nextDeferScope;
        for (StmtP& st : body) stmt(st.get());
        if (scopeHasDefer(deferScope)) {
            currentCode->emit(OP::END_DEFER); currentCode->u16(deferScope);
        }
        deferScope = outer;
        endScope();
    }
    void deferStmt(DeferStmt* d) {
        Expr* c = d->call.get();
        if (auto* call = dynamic_cast<Call*>(c)) {
            auto* fld = dynamic_cast<Field*>(call->callee.get());
            auto* base = fld ? dynamic_cast<Ident*>(fld->obj.get()) : nullptr;
            if (base && !modules.count(base->name) && !moduleVars.count(base->name)) {
                markDefer();                       // 表上的方法：t.f(...)
                expr(fld->obj.get());
                currentCode->emit(OP::LOAD_CONST); currentCode->u16(cp.addString(fld->name));
                currentCode->emit(OP::TABLE_GET);
                uint8_t argc = emitArgs(call->args);
                currentCode->emit(OP::DEFER); currentCode->u8(argc); currentCode->u16(deferScope);
                return;
            }
            if (base) {
                std::string key = base->name + "." + fld->name;
                uint8_t bid = builtinId(key);
                if (bid != 0xFF) { deferBuiltin(bid, call->args); return; }
                auto nit = natives.find(key);
                if (nit != natives.end()) { deferNative(key, call->args); return; }
                auto mi = modules.find(base->name);
                if (mi != modules.end() && mi->second.count(fld->name)) {
                    deferUser(mi->second[fld->name].funcId, call->args); return;
                }
                throw std::runtime_error("defer 后面必须是已定义的函数调用");
            }
            auto* id = dynamic_cast<Ident*>(call->callee.get());
            if (id) {
                uint8_t bid = builtinId(id->name);
                if (bid != 0xFF) { deferBuiltin(bid, call->args); return; }
                std::string key = curModule.empty() ? id->name : (curModule + "." + id->name);
                auto fit = syms.functions.find(key);
                if (fit != syms.functions.end()) { deferUser(fit->second, call->args); return; }
                auto mit = impFuncs.find(id->name);
                if (mit != impFuncs.end()) { deferUser(mit->second.funcId, call->args); return; }
                auto nit = natives.find(id->name);
                if (nit != natives.end()) { deferNative(id->name, call->args); return; }
            }
            // 闭包变量 / 任意表达式：DEFER 本来就吃"栈上的函数值"，直接支持
            markDefer();
            expr(call->callee.get());
            uint8_t argc = emitArgs(call->args);
            currentCode->emit(OP::DEFER); currentCode->u8(argc); currentCode->u16(deferScope);
            return;
        }
        throw std::runtime_error("defer 后面必须是一个函数调用");
    }
    // DEFER 是通用的：栈 [函数值, 参数...]，操作数 u8(argc) u16(scope)
    void deferUser(uint16_t fid, std::vector<ExprP>& args) {
        markDefer();
        emitFuncValue(fid);                      // 函数值
        uint8_t argc = emitArgs(args);
        currentCode->emit(OP::DEFER); currentCode->u8(argc); currentCode->u16(deferScope);
    }
    void deferBuiltin(uint8_t bid, std::vector<ExprP>& args) {
        markDefer();
        uint8_t argc = emitArgs(args);
        currentCode->emit(0x32); currentCode->u8(bid); currentCode->u8(argc);
        currentCode->u16(deferScope);
    }
    void deferNative(const std::string& key, std::vector<ExprP>& args) {
        markDefer();
        uint8_t argc = emitArgs(args);
        currentCode->emit(OP::DEFER_NATIVE); currentCode->u16(cp.addString(key));
        currentCode->u8(argc); currentCode->u16(deferScope);
    }

    // try/catch/finally：
    // · h1 = catch 的处理器（有 catch 才注册；否则它会抢在 finally 前截住异常）
    // · h2 = finally 的处理器（有 finally 才注册）
    // · finally 只有一份代码：异常路径先存错误 + 置标志，再回跳到 finally 体开头
    void tryStmt(TryStmt* t) {
        bool hasFin = t->hasFinally, hasCatch = t->hasCatch;
        if (!hasCatch && !hasFin) throw std::runtime_error("try 后面需要 catch 或 finally");

        syms.pushScope();
        uint16_t flagSlot = 0, errSlot = 0;
        std::string flagN = "$fin" + std::to_string(nextUserFuncId) + "_f";
        std::string errN  = "$fin" + std::to_string(nextUserFuncId) + "_e";
        if (hasFin) {
            flagSlot = syms.allocLocal(flagN);
            errSlot  = syms.allocLocal(errN);
            currentCode->emit(OP::FALSE_); genStore(flagSlot);
            emitNull(); genStore(errSlot);
        }
        size_t h2Land = 0;
        if (hasFin) { currentCode->emit(0x27); h2Land = placeholderU16(); }
        size_t h1Land = 0;
        if (hasCatch) { currentCode->emit(0x27); h1Land = placeholderU16(); }
        handlerDepth++;
        int savedFin = finGuard; finGuard = hasFin ? 1 : 0;
        scopedBlock(t->body);
        finGuard = savedFin;
        handlerDepth--;
        if (hasCatch) currentCode->emit(0x28);
        currentCode->emit(OP::JMP);
        size_t jmpFin = placeholderU16();
        if (hasCatch) patchU16(h1Land, jmpOffset(h1Land, currentCode->size()));

        if (hasCatch) {
            syms.pushScope();
            uint16_t slot = syms.allocLocal(t->catchVar);
            genStore(slot);
            int sf2 = finGuard; finGuard = hasFin ? 1 : 0;
            scopedBlock(t->catchBody);
            finGuard = sf2;
            syms.popScope();
        }
        patchU16(jmpFin, jmpOffset(jmpFin, currentCode->size()));

        size_t jmpDone = 0, finStart = 0;
        if (hasFin) {
            currentCode->emit(0x28);
            finStart = currentCode->size();
            scopedBlock(t->finallyBody);
            loadVar(flagN);
            currentCode->emit(OP::JZ);
            size_t jzSkip = placeholderU16();
            loadVar(errN);
            currentCode->emit(0x56);                 // THROW：原样重抛
            patchU16(jzSkip, jmpOffset(jzSkip, currentCode->size()));
            currentCode->emit(OP::JMP);
            jmpDone = placeholderU16();
            patchU16(h2Land, jmpOffset(h2Land, currentCode->size()));
            genStore(errSlot);
            currentCode->emit(OP::TRUE_); genStore(flagSlot);
            currentCode->emit(OP::JMP);
            size_t back = placeholderU16();
            patchU16(back, (uint16_t)((int64_t)finStart - (int64_t)(currentCode->size())));
        }
        syms.popScope();
        if (hasFin) patchU16(jmpDone, jmpOffset(jmpDone, currentCode->size()));
    }

    void localDecl(LocalDecl* l) {
        if (l->values.size() > l->names.size())
            throw std::runtime_error("多赋值时右侧表达式个数不能多于左侧目标个数");
        // 先算值再登记名字：`local x = x` 里的右侧要指向外层 x
        std::vector<RT> types;
        for (ExprP& v : l->values) types.push_back(expr(v.get()));
        // local a, b = f()：右侧就是一个调用 → want 改成目标个数
        if (inModuleInit) {
            // 模块变量是全局槽，即使此刻在函数帧里也必须走 STORE_G
            for (size_t i = 0; i < l->names.size(); i++) {
                uint16_t slot = syms.allocModuleVar(l->names[i]);
                if (i < l->values.size()) expr(l->values[i].get()); else emitNull();
                currentCode->emit(OP::STORE_G); currentCode->u16(slot);
            }
            return;
        }
        bool multi = (l->names.size() > 1 && l->values.size() == 1 &&
                      lastCallEndSize == currentCode->size() && lastCallWantPos != (size_t)-1);
        if (multi) currentCode->patchU8At(lastCallWantPos, (uint8_t)l->names.size());
        // 多返回值在栈上是 [v1, v2, ...]（最后一个在栈顶）→ 必须倒着存
        if (multi) {
            for (size_t i = l->names.size(); i-- > 0; ) {
                uint16_t slot = syms.allocLocal(l->names[i]);
                genStore(slot);
            }
            return;
        }
        for (size_t i = 0; i < l->names.size(); i++) {
            uint16_t slot = syms.allocLocal(l->names[i]);
            if (i < l->values.size()) genStore(slot);
            else { emitNull(); genStore(slot); }
        }
    }

    // 栈约定（TABLE_SET）：从底到顶 [表, 键, 值]，值在最上面。
    // 所以必须先压地址，再算值—— 旧实现先算全部值是错的，会把值压在最底下。
    void assign(Assign* a) {
        if (a->values.size() > a->targets.size())
            throw std::runtime_error("多赋值时右侧表达式个数不能多于左侧目标个数");
        bool multiA = (a->targets.size() > 1 && a->values.size() == 1 &&
                       dynamic_cast<Call*>(a->values[0].get()) != nullptr);
        // ① 多返回值：a, b = f() —— 值先算，再倒着存
        if (multiA) {
            bool allId2 = true;
            for (auto& t : a->targets) if (!dynamic_cast<Ident*>(t.get())) { allId2 = false; break; }
            if (allId2) {
                expr(a->values[0].get());
                if (lastCallEndSize == currentCode->size() && lastCallWantPos != (size_t)-1)
                    currentCode->patchU8At(lastCallWantPos, (uint8_t)a->targets.size());
                for (size_t i = a->targets.size(); i-- > 0; ) assignTo(a->targets[i].get());
                return;
            }
        }
        // ② 目标全是变量：先算全部值，再倒着存（a, b = b, a 才不会互相踩）
        bool allIdent = true;
        for (auto& t : a->targets) if (!dynamic_cast<Ident*>(t.get())) { allIdent = false; break; }
        if (allIdent) {
            for (ExprP& v : a->values) expr(v.get());
            size_t nv = a->values.size();
            for (size_t i = nv; i-- > 0; ) assignTo(a->targets[i].get());
            for (size_t i = nv; i < a->targets.size(); i++) { emitNull(); assignTo(a->targets[i].get()); }
            return;
        }
        // ③ 含容器目标：逐对"地址 + 值"
        for (size_t i = 0; i < a->targets.size(); i++) {
            Expr* t = a->targets[i].get();
            if (dynamic_cast<Ident*>(t)) {
                if (i < a->values.size()) expr(a->values[i].get()); else emitNull();
                assignTo(t);
                continue;
            }
            pushAddr(t);
            if (i < a->values.size()) expr(a->values[i].get()); else emitNull();
            currentCode->emit(OP::TABLE_SET);
        }
    }
    // 压入 [表, 键]（供 TABLE_SET / 复合赋值读取）
    void pushAddr(Expr* t) {
        if (auto* ix = dynamic_cast<Index*>(t)) { expr(ix->obj.get()); expr(ix->key.get()); return; }
        if (auto* f = dynamic_cast<Field*>(t)) {
            expr(f->obj.get());
            currentCode->emit(OP::LOAD_CONST); currentCode->u16(cp.addString(f->name));
            return;
        }
        throw std::runtime_error("赋值目标必须是变量 / 下标 / 字段");
    }

    void assignTo(Expr* t) {
        if (auto* id = dynamic_cast<Ident*>(t)) {
            VarInfo v = syms.resolve(id->name);
            if (v.kind == VarInfo::LOCAL) genStore(v.slot);
            else if (v.kind == VarInfo::UPVAL) {
                currentCode->emit(OP::SET_UPVAL); currentCode->u16(v.slot);
            } else {
                if (v.slot == (uint16_t)-1) v.slot = syms.allocGlobal(id->name);
                currentCode->emit(OP::STORE_G); currentCode->u16(v.slot);
            }
            return;
        }
        // 复合容器目标：调用方应走 pushAddr + 值 + TABLE_SET，这里不该到达
        throw std::runtime_error("赋值目标必须是变量");
    }

    void compound(CompoundAssign* c) {
        // a op= v ≡ a = a op v（同样：[表, 键, 值]）
        if (!dynamic_cast<Ident*>(c->target.get())) {
            uint16_t tt = syms.allocScratch();
            loadTarget(c->target.get());
            expr(c->value.get());
            switch (c->op) {
                case TK_ADDEQ: currentCode->emit(OP::ADD); break;
                case TK_SUBEQ: currentCode->emit(OP::SUB); break;
                case TK_MULEQ: currentCode->emit(OP::MUL); break;
                case TK_DIVEQ: currentCode->emit(OP::DIV); break;
            }
            genStore(tt);
            pushAddr(c->target.get());
            genLoad(tt);
            currentCode->emit(OP::TABLE_SET);
            return;
        }
        loadTarget(c->target.get());
        expr(c->value.get());
        switch (c->op) {
            case TK_ADDEQ: currentCode->emit(OP::ADD); break;
            case TK_SUBEQ: currentCode->emit(OP::SUB); break;
            case TK_MULEQ: currentCode->emit(OP::MUL); break;
            case TK_DIVEQ: currentCode->emit(OP::DIV); break;
        }
        assignTo(c->target.get());
    }

    void loadTarget(Expr* t) {
        if (auto* id = dynamic_cast<Ident*>(t)) { loadVar(id->name); return; }
        if (auto* ix = dynamic_cast<Index*>(t)) { expr(ix->obj.get()); expr(ix->key.get()); currentCode->emit(OP::TABLE_GET); return; }
        if (auto* f = dynamic_cast<Field*>(t)) {
            expr(f->obj.get());
            currentCode->emit(OP::LOAD_CONST); currentCode->u16(cp.addString(f->name));
            currentCode->emit(OP::TABLE_GET); return;
        }
    }

    void ifStmt(IfStmt* i) {
        std::vector<size_t> ends;
        auto branch = [&](Expr* cond, std::vector<StmtP>& body) {
            expr(cond);
            currentCode->emit(OP::JZ);
            size_t jz = placeholderU16();
            syms.pushScope();
            for (StmtP& s : body) stmt(s.get());
            endScope();
            currentCode->emit(OP::JMP);
            ends.push_back(placeholderU16());
            patchU16(jz, jmpOffset(jz, currentCode->size()));
        };
        branch(i->primary.cond.get(), i->primary.body);
        for (auto& b : i->elifs) branch(b.cond.get(), b.body);
        if (i->hasElse) {
            syms.pushScope();
            for (StmtP& s : i->elseBody) stmt(s.get());
            endScope();
        }
        for (size_t p : ends) patchU16(p, jmpOffset(p, currentCode->size()));
    }

    void whileStmt(WhileStmt* w) {
        size_t top = currentCode->size();
        uint16_t loopVarStart = syms.localCount;   // 循环体新分配的槽都要关上值
        expr(w->cond.get());
        currentCode->emit(OP::JZ);
        size_t jz = placeholderU16();
        bool savedLoop = inLoop; inLoop = true; loopDepth++;
        std::vector<size_t> sb, sc;
        std::swap(breakPatches, sb); std::swap(contPatches, sc);
        syms.pushScope();
        uint16_t ls = ++nextDeferScope, ods = deferScope; deferScope = ls;
        for (StmtP& s : w->body) stmt(s.get());
        // continue 的落点 = 本轮 END_DEFER 所在处（不是回跳点），否则 defer 会被跳过
        size_t wCont = currentCode->size();
        if (scopeHasDefer(ls)) { currentCode->emit(OP::END_DEFER); currentCode->u16(ls); }
        emitCloseScope(loopVarStart, syms.localCount);
        deferScope = ods;
        syms.popScope();
        currentCode->emit(OP::JMP);
        { size_t pp = placeholderU16(); patchU16(pp, jmpOffset(pp, top)); }
        patchU16(jz, jmpOffset(jz, currentCode->size()));
        collectOuter(breakPatches, outerBreaks);
        for (size_t p : breakPatches) patchU16(p, jmpOffset(p, currentCode->size()));
        collectOuter(contPatches, outerConts);
        for (size_t p : contPatches)  patchU16(p, jmpOffset(p, wCont));
        std::swap(breakPatches, sb); std::swap(contPatches, sc);
        inLoop = savedLoop; loopDepth--;
    }

    void doWhile(DoWhileStmt* d) {
        size_t top = currentCode->size();
        uint16_t loopVarStart = syms.localCount;
        bool savedLoop = inLoop; inLoop = true; loopDepth++;
        std::vector<size_t> sb, sc;
        std::swap(breakPatches, sb); std::swap(contPatches, sc);
        syms.pushScope();
        uint16_t ls = ++nextDeferScope, ods = deferScope; deferScope = ls;
        for (StmtP& s : d->body) stmt(s.get());
        if (scopeHasDefer(ls)) { currentCode->emit(OP::END_DEFER); currentCode->u16(ls); }
        emitCloseScope(loopVarStart, syms.localCount);
        deferScope = ods;
        endScope();
        size_t contTarget = currentCode->size();
        expr(d->cond.get());
        currentCode->emit(OP::JZ);
        size_t jz = placeholderU16();
        currentCode->emit(OP::JMP);
        { size_t pp = placeholderU16(); patchU16(pp, jmpOffset(pp, top)); }
        patchU16(jz, jmpOffset(jz, currentCode->size()));
        collectOuter(breakPatches, outerBreaks);
        for (size_t p : breakPatches) patchU16(p, jmpOffset(p, currentCode->size()));
        collectOuter(contPatches, outerConts);
        for (size_t p : contPatches)  patchU16(p, jmpOffset(p, contTarget));
        std::swap(breakPatches, sb); std::swap(contPatches, sc);
        inLoop = savedLoop; loopDepth--;
    }

    void forIn(ForInStmt* f) {
        syms.pushScope();
        uint16_t ls = ++nextDeferScope, ods = deferScope; deferScope = ls;
        uint16_t iterSlot = syms.allocScratch();
        expr(f->iter.get()); genStore(iterSlot);
        uint16_t keysSlot = syms.allocScratch();
        uint16_t cntSlot  = syms.allocScratch();
        uint16_t curSlot  = syms.allocScratch();
        genLoad(iterSlot); currentCode->emit(OP::TABLE_KEYS); genStore(keysSlot);
        genLoad(keysSlot); currentCode->emit(OP::TABLE_LEN);  genStore(cntSlot);
        emitInt(1); genStore(curSlot);
        size_t top = currentCode->size();
        genLoad(curSlot); genLoad(cntSlot);
        currentCode->emit(OP::CMP_LE);          // cur <= cnt 才继续
        currentCode->emit(OP::JZ);
        size_t jzEnd = placeholderU16();
        // 键总是要取（用来取值）；单变量时键落在隐藏槽，变量拿值
        uint16_t hiddenK = syms.allocScratch();
        uint16_t kSlot = syms.allocLocal(f->kName);
        uint16_t vSlot = f->vName.empty() ? 0xFFFF : syms.allocLocal(f->vName);
        if (vSlot == 0xFFFF) {
            genLoad(keysSlot); genLoad(curSlot); currentCode->emit(OP::TABLE_GET); genStore(hiddenK);
            genLoad(iterSlot); genLoad(hiddenK); currentCode->emit(OP::TABLE_GET); genStore(kSlot);
        } else {
            genLoad(keysSlot); genLoad(curSlot); currentCode->emit(OP::TABLE_GET); genStore(kSlot);
            genLoad(iterSlot); genLoad(kSlot); currentCode->emit(OP::TABLE_GET); genStore(vSlot);
        }
        bool savedLoop = inLoop; inLoop = true; loopDepth++;
        std::vector<size_t> sb, sc;
        std::swap(breakPatches, sb); std::swap(contPatches, sc);
        for (StmtP& s : f->body) stmt(s.get());
        if (scopeHasDefer(ls)) { currentCode->emit(OP::END_DEFER); currentCode->u16(ls); }
        // 先用本轮的值关掉上值再自增，否则闭包会看到 i+1
        emitCloseScope((uint16_t)(vSlot == 0xFFFF ? kSlot : kSlot), syms.localCount);
        deferScope = ods;
        size_t contTarget = currentCode->size();
        genLoad(curSlot); emitInt(1); currentCode->emit(OP::ADD); genStore(curSlot);
        currentCode->emit(OP::JMP);
        { size_t pp = placeholderU16(); patchU16(pp, jmpOffset(pp, top)); }
        patchU16(jzEnd, jmpOffset(jzEnd, currentCode->size()));
        collectOuter(breakPatches, outerBreaks);
        for (size_t p : breakPatches) patchU16(p, jmpOffset(p, currentCode->size()));
        collectOuter(contPatches, outerConts);
        for (size_t p : contPatches)  patchU16(p, jmpOffset(p, contTarget));
        std::swap(breakPatches, sb); std::swap(contPatches, sc);
        inLoop = savedLoop; loopDepth--;
        syms.popScope();
    }

    void forNum(ForNumStmt* f) {
        syms.pushScope();
        uint16_t ls = ++nextDeferScope, ods = deferScope; deferScope = ls;
        uint16_t iSlot = syms.allocLocal(f->name);
        uint16_t endSlot = syms.allocScratch();
        uint16_t stepSlot = syms.allocScratch();
        expr(f->start.get()); genStore(iSlot);
        expr(f->end.get());   genStore(endSlot);
        if (f->stepv) expr(f->stepv.get()); else emitInt(1);
        genStore(stepSlot);
        // 步长符号只在运行期可知（可能是变量），方向按 dir = step > 0 分支
        uint16_t dirSlot = syms.allocScratch();
        genLoad(stepSlot);
        currentCode->emit(OP::CALL); currentCode->u8(builtinId("float"));
        currentCode->u8(1); currentCode->u8(1);
        emitDouble(0.0);
        currentCode->emit(OP::CMP_GT);            // step > 0
        genStore(dirSlot);
        size_t top = currentCode->size();
        genLoad(dirSlot);
        currentCode->emit(OP::JZ);
        size_t jzNeg = placeholderU16();
        genLoad(iSlot); genLoad(endSlot); currentCode->emit(OP::CMP_LE);   // 正步长
        currentCode->emit(OP::JMP);
        size_t jmpCmp = placeholderU16();
        patchU16(jzNeg, jmpOffset(jzNeg, currentCode->size()));
        genLoad(iSlot); genLoad(endSlot); currentCode->emit(OP::CMP_GE);   // 负步长
        patchU16(jmpCmp, jmpOffset(jmpCmp, currentCode->size()));
        currentCode->emit(OP::JZ);
        size_t jzEnd = placeholderU16();
        bool savedLoop = inLoop; inLoop = true; loopDepth++;
        std::vector<size_t> sb, sc;
        std::swap(breakPatches, sb); std::swap(contPatches, sc);
        for (StmtP& s : f->body) stmt(s.get());
        if (scopeHasDefer(ls)) { currentCode->emit(OP::END_DEFER); currentCode->u16(ls); }
        emitCloseScope(iSlot, syms.localCount);        // 同上
        deferScope = ods;
        size_t contTarget = currentCode->size();
        genLoad(iSlot); genLoad(stepSlot); currentCode->emit(OP::ADD); genStore(iSlot);
        currentCode->emit(OP::JMP);
        { size_t pp = placeholderU16(); patchU16(pp, jmpOffset(pp, top)); }
        patchU16(jzEnd, jmpOffset(jzEnd, currentCode->size()));
        collectOuter(breakPatches, outerBreaks);
        for (size_t p : breakPatches) patchU16(p, jmpOffset(p, currentCode->size()));
        collectOuter(contPatches, outerConts);
        for (size_t p : contPatches)  patchU16(p, jmpOffset(p, contTarget));
        std::swap(breakPatches, sb); std::swap(contPatches, sc);
        inLoop = savedLoop; loopDepth--;
        syms.popScope();
    }

    void ret(ReturnStmt* r) {
        if (finGuard > 0)
            throw std::runtime_error(aePack(curFileIdx, r->line, r->col, 6,
                "try / catch 体里不能 return：它会跳过 finally 不执行（清理请改用 defer，或把 return 挪到 try 之后）"));
        for (ExprP& v : r->values) expr(v.get());
        // END_DEFER(0)：return 要把本帧所有defer 跑完（scope 0 = 全跑）
        // 同时注销本层 try 的处理器，否则异常表残留会让外层 catch 到错误的帧
        currentCode->emit(OP::END_DEFER); currentCode->u16(0);
        for (int i = 0; i < handlerDepth; i++) currentCode->emit(0x28);
        currentCode->emit(OP::RET);
        currentCode->u8((uint8_t)r->values.size());
    }

    // 表达式
    RT expr(Expr* e) {
        if (!e) { emitNull(); return RT::Null; }
        syncPos(e->line, e->col);
        if (auto* n = dynamic_cast<NumLit*>(e)) {
            currentCode->emit(OP::LOAD_CONST);
            currentCode->u16(n->isFloat ? cp.addDouble(n->d) : cp.addIntAuto(n->i));
            return n->isFloat ? RT::Double : RT::Int;
        }
        if (auto* s = dynamic_cast<StrLit*>(e)) {
            currentCode->emit(OP::LOAD_CONST);
            currentCode->u16(cp.addString(s->s)); return RT::Str;
        }
        if (auto* b = dynamic_cast<BoolLit*>(e)) {
            currentCode->emit(b->v ? OP::TRUE_ : OP::FALSE_); return RT::Bool;
        }
        if (dynamic_cast<NullLit*>(e)) { emitNull(); return RT::Null; }
        if (auto* id = dynamic_cast<Ident*>(e)) { loadVar(id->name); return RT::Null; }
        if (auto* u = dynamic_cast<Unary*>(e)) {
            if (u->op == TK_NOT) { expr(u->e.get()); currentCode->emit(OP::NOT_); return RT::Bool; }
            if (u->op == TK_TILDE) { expr(u->e.get()); currentCode->emit(OP::BNOT); return RT::Int; }
            if (u->op == TK_MINUS) {
                uint16_t tmp = syms.allocScratch();
                expr(u->e.get()); genStore(tmp);
                emitInt(0); genLoad(tmp);
                currentCode->emit(OP::SUB); return RT::Int;
            }
        }
        if (auto* b = dynamic_cast<Binary*>(e)) return binary(b);
        if (auto* t = dynamic_cast<Ternary*>(e)) {
            expr(t->c.get());
            currentCode->emit(OP::JZ);
            size_t jz = placeholderU16();
            expr(t->a.get());
            currentCode->emit(OP::JMP);
            size_t jm = placeholderU16();
            patchU16(jz, jmpOffset(jz, currentCode->size()));
            expr(t->b.get());
            patchU16(jm, jmpOffset(jm, currentCode->size()));
            return RT::Int;
        }
        if (auto* ix = dynamic_cast<Index*>(e)) {
            expr(ix->obj.get()); expr(ix->key.get());
            currentCode->emit(OP::TABLE_GET); return RT::Null;
        }
        if (auto* f = dynamic_cast<Field*>(e)) {
            auto* base = dynamic_cast<Ident*>(f->obj.get());
            if (base) {
                if (moduleMember(base->name, f->name)) return RT::Null;
                // 已知模块但取不到 → 精确报错（私有变量 / 不存在的函数）
                if (modules.count(base->name) || moduleVars.count(base->name))
                    noSuchMember(base->name, f->name);
            }
            expr(f->obj.get());
            currentCode->emit(OP::LOAD_CONST); currentCode->u16(cp.addString(f->name));
            currentCode->emit(OP::TABLE_GET); return RT::Null;
        }
        if (auto* c = dynamic_cast<Call*>(e))      return call(c);
        if (auto* sl = dynamic_cast<Slice*>(e))    { slice(sl); return RT::Null; }
        if (auto* ch = dynamic_cast<Chain*>(e))    { chain(ch); return RT::Bool; }
        if (auto* lam = dynamic_cast<Lambda*>(e))  { closure(lam); return RT::Func; }
        if (auto* cp2 = dynamic_cast<Comprehension*>(e)) { comprehension(cp2); return RT::Table; }
        if (auto* t = dynamic_cast<TableLit*>(e)) {
            // 栈约定：表始终在栈顶
            // 1) 算值 → 暂存 2) 算键 3) 取回值 4) TABLE_SET（表回到栈顶）
            // 数组段下标 = 项在字面量里的序号 + 1（与旧实现一致）
            currentCode->emit(OP::NEW_TABLE);
            currentCode->u16((uint16_t)t->items.size());
            uint16_t tmp = syms.allocScratch();
            for (size_t i = 0; i < t->items.size(); i++) {
                auto& it = t->items[i];
                expr(it.value.get());            // [ ... tbl v ]
                genStore(tmp);                   // [ ... tbl ]
                if (it.key) expr(it.key.get());  // [ ... tbl key ]
                else         emitInt((int64_t)i + 1);
                genLoad(tmp);                    // [ ... tbl key v ]
                currentCode->emit(OP::TABLE_SET);// [ ... tbl ]
            }
            return RT::Table;
        }
        return RT::Null;
    }

    RT binary(Binary* b) {
        // 逻辑运算：短路
        // 短路逻辑：结果保留操作数原值（Lua 语义），不是强制成 bool
        if (b->op == TK_OR) {
            uint16_t tmp = syms.allocScratch();
            expr(b->l.get()); genStore(tmp);
            genLoad(tmp);
            currentCode->emit(OP::JZ);            // 真 → 结果就是 a
            size_t jzB = placeholderU16();
            genLoad(tmp);
            currentCode->emit(OP::JMP);
            size_t jmE = placeholderU16();
            patchU16(jzB, jmpOffset(jzB, currentCode->size()));
            expr(b->r.get());                     // 假 → 结果取 b
            patchU16(jmE, jmpOffset(jmE, currentCode->size()));
            return RT::Null;
        }
        if (b->op == TK_AND) {
            uint16_t tmp = syms.allocScratch();
            expr(b->l.get()); genStore(tmp);
            genLoad(tmp);
            currentCode->emit(OP::JZ);            // 假 → 结果就是 a
            size_t jzK = placeholderU16();
            expr(b->r.get());                     // 真 → 结果取 b
            currentCode->emit(OP::JMP);
            size_t jmE = placeholderU16();
            patchU16(jzK, jmpOffset(jzK, currentCode->size()));
            genLoad(tmp);
            patchU16(jmE, jmpOffset(jmE, currentCode->size()));
            return RT::Null;
        }
        RT lt = expr(b->l.get());
        RT rt = expr(b->r.get());
        bool dbl = (lt == RT::Double || rt == RT::Double);
        switch (b->op) {
            // 统一严格比较：类型不同 → == 为 false、!= 为 true、大小比较一律 false
            case TK_EQ:  currentCode->emit(OP::CMP_EQ);  return RT::Bool;
            case TK_NEQ: currentCode->emit(OP::CMP_NEQ); return RT::Bool;
            case TK_LT:  currentCode->emit(OP::CMP_LT);  return RT::Bool;
            case TK_LE:  currentCode->emit(OP::CMP_LE);  return RT::Bool;
            case TK_GT:  currentCode->emit(OP::CMP_GT);  return RT::Bool;
            case TK_GE:  currentCode->emit(OP::CMP_GE);  return RT::Bool;
            case TK_PLUS:  currentCode->emit(dbl ? OP::ADD : OP::ADD); return dbl ? RT::Double : lt;
            case TK_MINUS: currentCode->emit(OP::SUB); return dbl ? RT::Double : lt;
            case TK_MUL:   currentCode->emit(OP::MUL); return dbl ? RT::Double : lt;
            case TK_DIV:   currentCode->emit(OP::DIV); return RT::Double;
            case TK_MOD:   currentCode->emit(OP::IMOD); return lt;
            case TK_POW:   currentCode->emit(OP::POW); return RT::Int;
            case TK_SHL:   currentCode->emit(OP::SHL); return RT::Int;
            case TK_SHR:   currentCode->emit(OP::SHR); return RT::Int;
            case TK_AMP:   currentCode->emit(OP::BAND); return RT::Int;
            case TK_PIPE:  currentCode->emit(OP::BOR);  return RT::Int;
            case TK_CARET: currentCode->emit(OP::BXOR); return RT::Int;
            default: break;
        }
        return RT::Int;
    }

    // 调用：优先级与旧 callExpr 一致
    // 限定名(mod.func) → 内置 → 本地 → 模块 → 原生 → 可调用变量（动态）
    RT call(Call* c, uint8_t want = 1) {
        // ① 限定名形态：mod.func(...)
        if (auto* fld = dynamic_cast<Field*>(c->callee.get())) {
            auto* base = dynamic_cast<Ident*>(fld->obj.get());
            if (base) return qualifiedCall(base->name, fld->name, c->args, want);
        }
        // ② 裸名
        if (auto* id = dynamic_cast<Ident*>(c->callee.get())) {
            const std::string& name = id->name;
            uint8_t bid = builtinId(name);
            if (bid != 0xFF) {
                ceLine = c->endLine; ceCol = c->endCol;
                emitBuiltin(bid, c->args, want); return RT::Null;
            }
            {
                VarInfo vi0 = syms.resolve(name);
                if (!inModuleInit && (vi0.kind == VarInfo::LOCAL || vi0.kind == VarInfo::UPVAL)) {
                    loadVar(name);
                    uint8_t argc = emitArgs(c->args);
                    currentCode->emit(OP::CALL_VALUE); currentCode->u8(argc); currentCode->u8(want);
                    noteCall(true);
                    return RT::Null;
                }
            }

            std::string key = curModule.empty() ? name : (curModule + "." + name);
            auto fit = syms.functions.find(key);
            if (fit == syms.functions.end() && !curModule.empty()) fit = syms.functions.find(name);
            if (fit != syms.functions.end()) {
                checkMinArgs(fit->second, c->args.size(), name, c->line, c->col);
                uint8_t argc = emitArgs(c->args);
                argc = fillDefaults(fit->second, argc, c->args.size());
                currentCode->emit(OP::CALL_FUNC);
                currentCode->u16(fit->second); currentCode->u8(argc); currentCode->u8(want);
                noteCall(true);
                return RT::Null;
            }
            auto mit = impFuncs.find(name);
            if (mit != impFuncs.end()) {
                checkMinArgs(mit->second.funcId, c->args.size(), name, c->line, c->col, mit->second.variadicSlot);
                uint8_t argc = emitArgs(c->args);
                argc = fillDefaults(mit->second.funcId, argc, c->args.size());
                currentCode->emit(OP::CALL_FUNC);
                currentCode->u16(mit->second.funcId); currentCode->u8(argc); currentCode->u8(want);
                noteCall(true);
                return RT::Null;
            }
            auto nit = natives.find(name);
            if (nit != natives.end()) { emitNative(name, nit->second, c->args, want); return RT::Null; }
            // 装着闭包的变量 → 动态调用
            VarInfo vi = syms.resolve(name);
            bool callable = (vi.kind == VarInfo::LOCAL) || (vi.kind == VarInfo::UPVAL) ||
                            (vi.kind == VarInfo::GLOBAL && vi.slot != (uint16_t)-1);
            if (callable) {
                loadVar(name);
                uint8_t argc = emitArgs(c->args);
                currentCode->emit(OP::CALL_VALUE); currentCode->u8(argc); currentCode->u8(want);
                return RT::Null;
            }
            throw std::runtime_error("未定义的函数: " + name +
                "（若它来自原生库，请先在文件里写 imp <库名>）");
        }
        // ③ 任意表达式 → 动态调用（栈序 [函数值, 参数...]）
        expr(c->callee.get());
        uint8_t argc = emitArgs(c->args);
        currentCode->emit(OP::CALL_VALUE); currentCode->u8(argc); currentCode->u8(want);
        noteCall(true);
        return RT::Null;
    }

    RT qualifiedCall(const std::string& mod, const std::string& fn,
                     std::vector<ExprP>& args, uint8_t want) {
        std::string key = mod + "." + fn;
        uint8_t bid = builtinId(key);
        if (bid != 0xFF) { emitBuiltin(bid, args, want); return RT::Null; }
        auto nit = natives.find(key);
        if (nit != natives.end()) { emitNative(key, nit->second, args, want); return RT::Null; }
        auto mi = modules.find(mod);
        if (mi != modules.end()) {
            auto fi = mi->second.find(fn);
            if (fi != mi->second.end()) {
                checkMinArgs(fi->second.funcId, args.size(), fn, 0, 0, fi->second.variadicSlot);
                uint8_t argc = emitArgs(args);
                currentCode->emit(OP::CALL_FUNC);
                currentCode->u16(fi->second.funcId); currentCode->u8(argc); currentCode->u8(want);
                noteCall(true);
                return RT::Null;
            }
        }
        // 不是库也不是模块 → 当成"表/变量上的字段调用"：obj.fn(...)
        // 栈序 [函数值, 参数...] → CALL_VALUE
        if (syms.resolve(mod).kind != VarInfo::GLOBAL || syms.resolve(mod).slot != (uint16_t)-1) {
            loadVar(mod);
            currentCode->emit(OP::LOAD_CONST); currentCode->u16(cp.addString(fn));
            currentCode->emit(OP::TABLE_GET);
            uint8_t argc = emitArgs(args);
            currentCode->emit(OP::CALL_VALUE); currentCode->u8(argc); currentCode->u8(want);
            return RT::Null;
        }
        throw std::runtime_error("库 '" + mod + "' 里没有函数 '" + key + "'");
    }

    // 默认参数：实参不够时，按位置补上默认值常量（变参函数不补）
    std::map<uint16_t, std::vector<uint16_t>> funcDefaults;
    std::map<uint16_t, uint16_t> funcVariadic;   // fid → 变参槽（0xFFFF = 无变参）
    uint8_t fillDefaults(uint16_t fid, uint8_t argc, size_t given) {
        auto it = funcDefaults.find(fid);
        if (it == funcDefaults.end()) return argc;
        const std::vector<uint16_t>& dc = it->second;
        for (size_t i = given; i < dc.size(); i++) {
            if (dc[i] == 0xFFFF || dc[i] == 0xFFFC) continue;   // 必传位 / 变参位：不补
            currentCode->emit(OP::LOAD_CONST);
            currentCode->u16(dc[i] == 0xFFFE ? 0 : dc[i]);   // 见下：bool 走专用指令
            if (dc[i] == 0xFFFE || dc[i] == 0xFFFD) {
                currentCode->code.pop_back(); currentCode->code.pop_back();
                currentCode->code.pop_back();
                currentCode->emit(dc[i] == 0xFFFE ? OP::TRUE_ : OP::FALSE_);
            }
            argc++;
        }
        return argc;
    }
    // 多返回值：local a, b = f() 时要把调用的 want 改成 2。
    // 记录"最近一次调用的 want 字节位置 + 发射结束时的代码长度"，
    // 赋值语句发现"右侧就是这一个调用"时回填。（与旧编译器的 lastCallWantPos 同思路）
    size_t lastCallWantPos = (size_t)-1;
    int ceLine = 0, ceCol = 0;   // 本次调用的结束位置（')'），emit CALL 前落账
    size_t lastCallEndSize = (size_t)-1;
    void noteCall(bool onStack) {
        lastCallWantPos = onStack ? (currentCode->size() - 1) : (size_t)-1;
        lastCallEndSize = currentCode->size();
    }
    // 调用点参数个数检查：默认值/变参放宽了上限，但最少几个仍然编译期检查
    void checkMinArgs(uint16_t fid, size_t argc, const std::string& fname, int line, int col,
                      uint16_t vslot = 0xFFFF) {
        // 最少参数数 = 没有默认值的参数个数（变参位本身不算必传）
        uint16_t vs = vslot;
        if (vs == 0xFFFF) { auto vit = funcVariadic.find(fid); if (vit != funcVariadic.end()) vs = vit->second; }
        uint16_t minArgs = 0;
        auto dit = funcDefaults.find(fid);
        if (dit != funcDefaults.end()) {
            for (size_t i = 0; i < dit->second.size(); i++) {
                if ((uint16_t)i == vs) continue;                 // 变参位跳过
                if (dit->second[i] == 0xFFFF) minArgs++;
            }
        }
        if (minArgs == 0) return;
        if ((uint16_t)argc < minArgs)
            throw std::runtime_error(aePack(curFileIdx, line, col, (int)fname.size(),
                "函数 '" + fname + "' 至少需要 " + std::to_string(minArgs) +
                " 个参数，实际给了 " + std::to_string(argc)));
    }
    uint8_t emitArgs(std::vector<ExprP>& args) {
        for (ExprP& a : args) expr(a.get());
        return (uint8_t)args.size();
    }
    void emitBuiltin(uint8_t bid, std::vector<ExprP>& args, uint8_t want) {
        uint8_t argc = emitArgs(args);
        if (ceCol) { syncPos(ceLine, ceCol); ceCol = 0; }   // 参数算完后再落账，否则被覆盖
        currentCode->emit(OP::CALL);
        currentCode->u8(bid); currentCode->u8(argc); currentCode->u8(want);
        noteCall(true);
    }
    void emitNative(const std::string& key, const NativeInfo& info,
                    std::vector<ExprP>& args, uint8_t want) {
        if ((int)args.size() < info.minArgs)
            throw std::runtime_error("库函数 " + key + " 至少需要 " + std::to_string(info.minArgs) +
                                     " 个参数，实际给了 " + std::to_string(args.size()));
        if (info.maxArgs >= 0 && (int)args.size() > info.maxArgs)
            throw std::runtime_error("库函数 " + key + " 最多接受 " + std::to_string(info.maxArgs) +
                                     " 个参数，实际给了 " + std::to_string(args.size()));
        uint8_t argc = emitArgs(args);
        currentCode->emit(OP::NATIVE);
        currentCode->u16(cp.addString(key)); currentCode->u8(argc); currentCode->u8(want);
        noteCall(true);
    }

    // 切片：栈序 [容器, start, end, step]，省略的段补 null
    void slice(Slice* sl) {
        expr(sl->obj.get());
        if (sl->start) expr(sl->start.get()); else emitNull();
        if (sl->end)   expr(sl->end.get());   else emitNull();
        if (sl->step)  expr(sl->step.get());  else emitNull();
        currentCode->emit(OP::SLICE);
    }

    // 链式比较：中间操作数只求值一次（展开成 and 会求值两遍）
    void chain(Chain* ch) {
        if (ch->ops.size() == 1) {                    // 单段：与旧路径一致
            expr(ch->operands[0].get());
            expr(ch->operands[1].get());
            if (ch->ops[0] == TK_IN) { currentCode->emit(OP::IN); }
            else emitCmpOp(ch->ops[0]);
            if (ch->negates[0]) currentCode->emit(OP::NOT_);
            return;
        }
        uint16_t prev = syms.allocScratch(), cur = syms.allocScratch();
        expr(ch->operands[0].get()); genStore(prev);
        std::vector<size_t> falsePatches;
        for (size_t i = 0; i < ch->ops.size(); i++) {
            expr(ch->operands[i + 1].get()); genStore(cur);
            genLoad(prev); genLoad(cur);
            if (ch->ops[i] == TK_IN) currentCode->emit(OP::IN);
            else emitCmpOp(ch->ops[i]);
            if (ch->negates[i]) currentCode->emit(OP::NOT_);
            if (i + 1 == ch->ops.size()) break;
            currentCode->emit(OP::JZ);
            falsePatches.push_back(placeholderU16());
            genLoad(cur); genStore(prev);
        }
        if (falsePatches.empty()) return;
        currentCode->emit(OP::JMP);
        size_t jm = placeholderU16();
        size_t lFalse = currentCode->size();
        for (size_t p : falsePatches) patchU16(p, jmpOffset(p, lFalse));
        currentCode->emit(OP::FALSE_);
        patchU16(jm, jmpOffset(jm, currentCode->size()));
    }
    void emitCmpOp(int op) {
        switch (op) {
            case TK_EQ:  currentCode->emit(OP::CMP_EQ);  break;
            case TK_NEQ: currentCode->emit(OP::CMP_NEQ); break;
            case TK_LT:  currentCode->emit(OP::CMP_LT);  break;
            case TK_LE:  currentCode->emit(OP::CMP_LE);  break;
            case TK_GT:  currentCode->emit(OP::CMP_GT);  break;
            case TK_GE:  currentCode->emit(OP::CMP_GE);  break;
            default: break;
        }
    }

    // 推导：编译期展开成等价的嵌套循环（不新增指令，VM 不知道推导存在过）
    void comprehension(Comprehension* c) {
        syms.pushScope();
        uint16_t resultSlot = syms.allocScratch();
        uint16_t idxSlot    = syms.allocScratch();
        currentCode->emit(OP::NEW_TABLE); currentCode->u16(0);
        genStore(resultSlot);
        if (!c->isDict) { emitInt(1); genStore(idxSlot); }
        std::vector<size_t> contPatches;
        genComprLevel(c, 0, resultSlot, idxSlot, nullptr);
        genLoad(resultSlot);
        syms.popScope();
        (void)contPatches;
    }

    void genComprLevel(Comprehension* c, size_t i, uint16_t resultSlot, uint16_t idxSlot,
                       std::vector<size_t>* contPatches) {
        if (i >= c->items.size()) {
            genLoad(resultSlot);
            if (c->isDict) {
                if (c->key) expr(c->key.get());
                if (c->value) expr(c->value.get());
            } else {
                genLoad(idxSlot);
                if (c->value) expr(c->value.get());
            }
            currentCode->emit(OP::TABLE_SET);
            currentCode->emit(OP::POP);
            if (!c->isDict) {
                genLoad(idxSlot); emitInt(1); currentCode->emit(OP::ADD); genStore(idxSlot);
            }
            return;
        }
        ComprItem& it = c->items[i];
        if (it.isNumeric) {
            uint16_t iSlot = syms.allocLocal(it.nName);
            uint16_t endSlot = syms.allocScratch();
            uint16_t stepSlot = syms.allocScratch();
            if (it.start) expr(it.start.get()); else emitInt(1);
            genStore(iSlot);
            if (it.stop) expr(it.stop.get()); else emitInt(0);
            genStore(endSlot);
            if (it.stepv) expr(it.stepv.get()); else emitInt(1);
            genStore(stepSlot);
            uint16_t dirSlot = syms.allocScratch();
            genLoad(stepSlot);
            currentCode->emit(OP::CALL); currentCode->u8(builtinId("float"));
            currentCode->u8(1); currentCode->u8(1);
            emitDouble(0.0);
            currentCode->emit(OP::CMP_GT);
            genStore(dirSlot);
            size_t top = currentCode->size();
            genLoad(dirSlot);
            currentCode->emit(OP::JZ);
            size_t jzNeg = placeholderU16();
            genLoad(iSlot); genLoad(endSlot); currentCode->emit(OP::CMP_LE);
            currentCode->emit(OP::JMP);
            size_t jmpCmp = placeholderU16();
            patchU16(jzNeg, jmpOffset(jzNeg, currentCode->size()));
            genLoad(iSlot); genLoad(endSlot); currentCode->emit(OP::CMP_GE);
            patchU16(jmpCmp, jmpOffset(jmpCmp, currentCode->size()));
            currentCode->emit(OP::JZ);
            size_t jzEnd = placeholderU16();
            if (it.cond) {
                expr(it.cond.get());
                currentCode->emit(OP::JZ);
                size_t jzC = placeholderU16();
                genComprLevel(c, i + 1, resultSlot, idxSlot, nullptr);
                emitCloseScope(iSlot, syms.localCount);
                patchU16(jzC, jmpOffset(jzC, currentCode->size()));
            } else {
                genComprLevel(c, i + 1, resultSlot, idxSlot, nullptr);
                emitCloseScope(iSlot, syms.localCount);
            }
            size_t contTarget = currentCode->size();
            genLoad(iSlot); genLoad(stepSlot); currentCode->emit(OP::ADD); genStore(iSlot);
            currentCode->emit(OP::JMP);
            { size_t pp = placeholderU16(); patchU16(pp, jmpOffset(pp, top)); }
            patchU16(jzEnd, jmpOffset(jzEnd, currentCode->size()));
            (void)contTarget;
            return;
        }
        // for-in
        uint16_t iterSlot = syms.allocScratch();
        if (it.iter) expr(it.iter.get()); else { emitNull(); }
        genStore(iterSlot);
        uint16_t keysSlot = syms.allocScratch();
        uint16_t cntSlot  = syms.allocScratch();
        uint16_t curSlot  = syms.allocScratch();
        genLoad(iterSlot); currentCode->emit(OP::TABLE_KEYS); genStore(keysSlot);
        genLoad(keysSlot); currentCode->emit(OP::TABLE_LEN);  genStore(cntSlot);
        emitInt(1); genStore(curSlot);
        size_t top = currentCode->size();
        genLoad(curSlot); genLoad(cntSlot);
        currentCode->emit(OP::CMP_LE);          // cur <= cnt 才继续
        currentCode->emit(OP::JZ);
        size_t jzEnd = placeholderU16();
        uint16_t hiddenK = syms.allocScratch();
        uint16_t kSlot = syms.allocLocal(it.kName);
        uint16_t cStart = kSlot;
        uint16_t vSlot = it.vName.empty() ? 0xFFFF : syms.allocLocal(it.vName);
        if (vSlot == 0xFFFF) {
            genLoad(keysSlot); genLoad(curSlot); currentCode->emit(OP::TABLE_GET); genStore(hiddenK);
            genLoad(iterSlot); genLoad(hiddenK); currentCode->emit(OP::TABLE_GET); genStore(kSlot);
        } else {
            genLoad(keysSlot); genLoad(curSlot); currentCode->emit(OP::TABLE_GET); genStore(kSlot);
            genLoad(iterSlot); genLoad(kSlot); currentCode->emit(OP::TABLE_GET); genStore(vSlot);
        }
        if (it.cond) {
            expr(it.cond.get());
            currentCode->emit(OP::JZ);
            size_t jzC = placeholderU16();
            genComprLevel(c, i + 1, resultSlot, idxSlot, nullptr);
            emitCloseScope(cStart, syms.localCount);
            patchU16(jzC, jmpOffset(jzC, currentCode->size()));
        } else {
            genComprLevel(c, i + 1, resultSlot, idxSlot, nullptr);
            emitCloseScope(cStart, syms.localCount);
        }
        size_t contTarget = currentCode->size();
        genLoad(curSlot); emitInt(1); currentCode->emit(OP::ADD); genStore(curSlot);
        currentCode->emit(OP::JMP);
        { size_t pp = placeholderU16(); patchU16(pp, jmpOffset(pp, top)); }
        patchU16(jzEnd, jmpOffset(jzEnd, currentCode->size()));
        (void)contTarget; (void)contPatches;
    }

    // 闭包：独立函数体 + CLOSURE 指令（上值由符号表的 ctx 机制解析）
    void closure(Lambda* lam) {
        uint16_t fid = nextUserFuncId++;
        std::string savedModule = curModule;
        // 自引用名：先登记局部，让体内能调到自己
        uint16_t selfSlot = 0xFFFF;
        if (!lam->name.empty()) selfSlot = syms.allocLocal(lam->name);  // 外层槽 → 体里当上值
        syms.pushCtx("<lambda>");
        CodeBuf bodyBuf; CodeBuf* saved = currentCode;
        int savedHD = handlerDepth; handlerDepth = 0;
        currentCode = &bodyBuf;
        bodyBuf.markLine((uint32_t)lam->line);
        std::vector<uint16_t> defaultConsts; uint16_t variadicSlot = 0xFFFF;
        for (const Param& pm : lam->params) {
            uint16_t slot = syms.allocParam(pm.name);
            if (pm.variadic) { variadicSlot = slot; defaultConsts.push_back(0xFFFF); continue; }
            defaultConsts.push_back(pm.def ? constIdxOf(pm.def.get()) : 0xFFFF);
        }
        for (StmtP& st : lam->body) stmt(st.get());
        bodyBuf.emit(OP::END_DEFER); bodyBuf.u16(0);
        bodyBuf.emit(OP::RET); bodyBuf.u8(0);
        currentCode = saved; handlerDepth = savedHD;
        uint16_t frameLocals = syms.maxLocalCount;
        std::vector<UpvalDesc> ups; syms.popCtx(&ups);
        // 自引用：把闭包自身写进它的槽（用 GET_UPVAL 拿不到，改用 CLOSURE 后存局部）
        FuncBlob fb;
        fb.code = bodyBuf.code; fb.lines = bodyBuf.lines;
        fb.name = lam->name.empty() ? "<lambda>" : lam->name;
        fb.funcId = fid; fb.paramCount = (uint16_t)lam->params.size();
        fb.localCount = frameLocals; fb.upvals = ups;
        fb.defaultConsts = defaultConsts; fb.variadicSlot = variadicSlot;
        funcDefaults[fid] = defaultConsts; funcVariadic[fid] = variadicSlot;
        funcDefs.push_back(std::move(fb));
        syms.closureCount++;
        currentCode->emit(OP::CLOSURE);
        currentCode->u16(fid); currentCode->u8((uint8_t)ups.size());
        if (selfSlot != 0xFFFF) { genStore(selfSlot); genLoad(selfSlot); }
        curModule = savedModule;
    }

    // 模块成员取用：先查模块函数（可当值传出），再查模块变量
    bool moduleMember(const std::string& mod, const std::string& mem) {
        auto mi = modules.find(mod);
        if (mi != modules.end()) {
            auto fi = mi->second.find(mem);
            if (fi != mi->second.end()) { emitFuncValue(fi->second.funcId); return true; }
        }
        auto vi = moduleVars.find(mod);
        if (vi != moduleVars.end()) {
            auto si = vi->second.find(mem);
            if (si != vi->second.end() && si->second != (uint16_t)-1) {
                currentCode->emit(OP::LOAD_G); currentCode->u16(si->second); return true;
            }
        }
        return false;
    }
    // 限定名取用失败时给精确报错：区分"库里没有这个函数"和"模块没导出这个变量"
    [[noreturn]] void noSuchMember(const std::string& mod, const std::string& mem) {
        auto vi = moduleVars.find(mod);
        bool knownVar = false;
        if (vi != moduleVars.end()) {
            std::string key = mod + "." + mem;
            knownVar = syms.globals.count(key) != 0;   // 存在但没导出 = 私有
        }
        if (knownVar)
            throw std::runtime_error("库 '" + mod + "' 里没有导出的 '" + mem + "'");
        throw std::runtime_error("库 '" + mod + "' 里没有函数 '" + mod + "." + mem + "'");
    }
    void emitFuncValue(uint16_t fid) {
        currentCode->emit(OP::CLOSURE); currentCode->u16(fid); currentCode->u8(0);
        syms.closureCount++;
    }
    void loadVar(const std::string& name) {
        VarInfo v = syms.resolve(name);
        if (v.kind == VarInfo::LOCAL) { genLoad(v.slot); return; }
        if (v.kind == VarInfo::UPVAL) { currentCode->emit(OP::GET_UPVAL); currentCode->u16(v.slot); return; }
        if (v.slot != (uint16_t)-1) { currentCode->emit(OP::LOAD_G); currentCode->u16(v.slot); return; }
        // 顶层赋过名（但这次没走到赋值）→ 按全局变量读，值是 null。
        // 旧编译器靠 assignedNames 做同样的事（`块内 z 已不可见` 期望 null 而不是报错）
        if (assignedNames.count(name)) {
            uint16_t g = syms.allocGlobal(name);
            currentCode->emit(OP::LOAD_G); currentCode->u16(g); return;
        }
        // 具名函数当值用（func_value）：函数表命中就发一条 CLOSURE
        {
            std::string key = curModule.empty() ? name : (curModule + "." + name);
            auto fit = syms.functions.find(key);
            if (fit == syms.functions.end() && !curModule.empty()) fit = syms.functions.find(name);
            if (fit != syms.functions.end()) { emitFuncValue(fit->second); return; }
            auto mit = impFuncs.find(name);
            if (mit != impFuncs.end()) { emitFuncValue(mit->second.funcId); return; }
        }
        throw std::runtime_error("未声明的变量 '" + name + "'");
    }

    // 发射辅助
    // 带层数的 break/continue：level>1 时按"目标循环深度"暂存，等那层循环退出时再修补。
    // 循环进入 depth++、退出 depth--；break 2 的目标深度 = depth - 1。
    int loopDepth = 0;
    std::map<int, std::vector<size_t>> outerBreaks, outerConts;
    std::vector<size_t> breakPatches, contPatches;
    void collectOuter(std::vector<size_t>& dst, std::map<int, std::vector<size_t>>& src) {
        auto it = src.find(loopDepth);
        if (it == src.end()) return;
        dst.insert(dst.end(), it->second.begin(), it->second.end());
        src.erase(it);
    }

    size_t placeholderU16() { currentCode->u16(0); return currentCode->size() - 2; }
    void patchU16(size_t pos, uint16_t val) { currentCode->patchU16At(pos, val); }
    uint16_t jmpOffset(size_t placeholderPos, size_t targetPos) {
        long d = (long)targetPos - (long)(placeholderPos + 2);
        return (uint16_t)(int16_t)d;
    }
    // 关闭上值：让循环每轮的闭包拿到自己的变量副本。
    // 省掉的条件（与旧实现一致）：本函数体没建过闭包 → 不可能有打开的上值。
    void emitCloseScope(uint16_t from, uint16_t to) {
        if (to <= from) return;
        if (syms.closureCount == 0) return;
        for (uint16_t sl = from; sl < to; sl++) {
            currentCode->emit(OP::CLOSE_SLOT); currentCode->u16(sl);
        }
    }
    void endScope() {
        if (!syms.scopes.empty())
            emitCloseScope(syms.scopes.back().startSlot, syms.localCount);
        syms.popScope();
    }
    void genStore(uint16_t slot) {
        if (syms.inFunction) { currentCode->emit(OP::STORE_L); currentCode->u16(slot); }
        else                 { currentCode->emit(OP::STORE_G); currentCode->u16(slot); }
    }
    void genLoad(uint16_t slot) {
        if (syms.inFunction) { currentCode->emit(OP::LOAD_L); currentCode->u16(slot); }
        else                 { currentCode->emit(OP::LOAD_G); currentCode->u16(slot); }
    }
    void emitNull() { currentCode->emit(OP::LOAD_CONST); currentCode->u16(cp.addNull()); }
    void emitInt(int64_t v) { currentCode->emit(OP::LOAD_CONST); currentCode->u16(cp.addIntAuto(v)); }
    void emitDouble(double v) { currentCode->emit(OP::LOAD_CONST); currentCode->u16(cp.addDouble(v)); }

    uint8_t builtinId(const std::string& name) const {
        static const std::map<std::string, uint8_t> k = {
            {"pr",0},{"inp",1},{"prln",2},{"len",3},{"str",4},{"int",5},{"float",6},{"type",7},
            {"assert",8},{"error",9},{"exit",10},{"substr",11},{"ord",12},{"chr",13},{"find",14},
            {"upper",15},{"lower",16},{"trim",17},{"replace",18},{"split",19},{"join",20},
            {"repeat",21},{"format",22},{"delete",23},{"insert",24},{"pop",25},{"append",26},
            {"count",27},{"copy",28},{"deepcopy",29},{"equal",30},{"raise",31},{"run",32},
            {"weak",33},{"collect",34},{"gcstats",35},{"sort",36},{"sortBy",37},
            {"coroutine.create",38},{"coroutine.resume",39},{"coroutine.yield",40},
            {"coroutine.status",41},{"coroutine.close",42},
        };
        auto it = k.find(name);
        return it == k.end() ? (uint8_t)0xFF : it->second;
    }
};

// 序列化：写出 .aeo 文件
inline void emitbcSave(Emitter& E, const std::string& path, const std::string& srcName) {
    std::vector<uint8_t> out;
    out.insert(out.end(), {'A','e','B','c'});
    out.push_back(1); out.push_back(8); out.push_back(0); out.push_back(0);
    E.cp.write(out);
    writeU32(out, (uint32_t)srcName.size());
    out.insert(out.end(), srcName.begin(), srcName.end());
    writeU32(out, (uint32_t)E.fileNames.size());
    for (const std::string& fn : E.fileNames) {
        writeU32(out, (uint32_t)fn.size());
        out.insert(out.end(), fn.begin(), fn.end());
    }
    writeU32(out, (uint32_t)E.funcNames.size());
    for (const auto& fn : E.funcNames) {
        writeU16(out, fn.first);
        writeU32(out, (uint32_t)fn.second.size());
        out.insert(out.end(), fn.second.begin(), fn.second.end());
    }
    writeU32(out, (uint32_t)E.allLines.size());
    for (const auto& L : E.allLines) {
        writeU32(out, L.off); writeU32(out, L.line); writeU32(out, L.col); writeU16(out, L.file);
    }
    E.output.write(out);
    writeU32(out, (uint32_t)E.importedModules.size());
    for (const std::string& m : E.importedModules) {
        writeU32(out, (uint32_t)m.size());
        out.insert(out.end(), m.begin(), m.end());
    }
    if (!aepath::writeFileBytes(path, out.data(), out.size()))
        throw std::runtime_error("无法写入输出文件: " + path);
}

} // namespace aeemit
