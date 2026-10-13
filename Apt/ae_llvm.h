// ae_llvm.h —— AOT 后端的进程内 C++ 编译器（Clang + LLD 作为库）
// 为什么必须是库、而不能进程外调用编译器：
// Ae 要在安卓（C4Droid / Termux）上跑。安卓的共享存储是 noexec，
// 应用私有目录里能执行单个二进制，但再 fork 出第二个二进制会撞权限。
// 把 Clang 与 LLD 链进 aec 本体，"源码 → 可执行文件"就全在自己进程内完成，
// 不需要任何外部进程 —— 这样才能做到全平台自包含。
//
// 两条库调用：
// 1) Clang 前端 + CodeGen：.cpp → .o （EmitObjAction，进程内）
// 2) LLD： .o → 可执行文件 （lld::elf::link，进程内）
//
// 链接参数由 Clang 的 Driver 生成、由 LLD 执行（见 driverLinkArgs）。
// 这是安卓上能跑通的关键：sysroot / crt / linker64 只有 Driver 会算。
//
// 未启用时（没找到 llvm-config）本文件只提供空壳，行为由 aec 的回退链兜住。
#ifndef AE_LLVM_H
#define AE_LLVM_H

#include <string>
#include <vector>
#include <cstdio>

namespace aellvm {

// 本份 aec 是否真的链进了 Clang/LLD
inline bool available() {
#ifdef AE_ENABLE_LLVM
    return true;
#else
    return false;
#endif
}

// 未启用时给用户的说明
inline const char* unavailableReason() {
#ifdef AE_ENABLE_LLVM
    return "";
#else
    return "本份 aec 未链入 Clang/LLD（构建时没找到 llvm-config）。\n"
           "       = help: 装好 LLVM 后重编：./build.sh（会自动探测 llvm-config）\n"
           "       = help: 或先用 --aot 生成 .cpp，再手动用 g++/clang++ 编译";
#endif
}

// 编译结果
struct Result {
    bool ok = false;
    std::string stage;      // "compile" / "link"
    std::string message;
};

} // namespace aellvm

#ifdef AE_ENABLE_LLVM

#include <memory>
#include <iterator>
#include <algorithm>
#include <type_traits>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/VirtualFileSystem.h>
// LLVM 20 起 Host.h 从 Support/ 移到了 TargetParser/
#if LLVM_VERSION_MAJOR >= 20
#include <llvm/TargetParser/Host.h>
#else
#include <llvm/Support/Host.h>
#endif
#include <llvm/Config/llvm-config.h>

#include <clang/Basic/DiagnosticOptions.h>
#include <clang/Basic/DiagnosticIDs.h>
#include <clang/Driver/Driver.h>
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Job.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <clang/Frontend/Utils.h>
#include <clang/CodeGen/CodeGenAction.h>

// LLD 声明
// 故意不 includelld/Common/Driver.h，一律自己声明。
//
// 原因：LLVM 21 的那个头文件里，elf / coff / macho 三个命名空间是用宏
// LLD_DEFINE_LINK(name) 生成的，只有宏被实际调用时命名空间才存在。
// Termux 的这份头里宏没被调用 → 报 "no member named 'elf' in namespace 'lld'"。
//
// 签名照抄头文件里的宏体（5 参数），与 liblldELF.a 里的真实符号一致：
// link(args, stdoutOS, stderrOS, exitEarly, disableOutput)
namespace lld {
namespace elf {
bool link(llvm::ArrayRef<const char*> args, llvm::raw_ostream& stdoutOS,
          llvm::raw_ostream& stderrOS, bool exitEarly, bool disableOutput);
}
namespace coff {
bool link(llvm::ArrayRef<const char*> args, llvm::raw_ostream& stdoutOS,
          llvm::raw_ostream& stderrOS, bool exitEarly, bool disableOutput);
}
namespace macho {
bool link(llvm::ArrayRef<const char*> args, llvm::raw_ostream& stdoutOS,
          llvm::raw_ostream& stderrOS, bool exitEarly, bool disableOutput);
}
}

namespace aellvm {

// 编译期探测：CompilerInstance 有没有 setDiagnostics
// 各版本签名不一样，用 SFINAE 自动选，避免写死版本判断。
template <class T, class = void>
struct HasSetDiagnostics : std::false_type {};
template <class T>
struct HasSetDiagnostics<T, std::void_t<decltype(
    std::declval<T&>().setDiagnostics(nullptr))>> : std::true_type {};

// Clang 可执行文件的真实路径（构建时 -D 烘焙）。
// 必须给真路径：Driver 靠它推导 sysroot / 资源目录 / crt 在哪。
#ifdef AE_CLANG_BINDIR
inline std::string clangBinary() { return AE_CLANG_BINDIR "/clang++"; }
#else
inline std::string clangBinary() { return "clang++"; }
#endif

// 诊断收集器：把 Clang 报错攒成字符串，交给 Ae 的诊断系统渲染
struct DiagSaver : clang::DiagnosticConsumer {
    std::string message;
    llvm::raw_string_ostream os{message};
    int errorCount = 0;

    void HandleDiagnostic(clang::DiagnosticsEngine::Level lv,
                          const clang::Diagnostic& info) override {
        clang::DiagnosticConsumer::HandleDiagnostic(lv, info);
        if (lv == clang::DiagnosticsEngine::Error ||
            lv == clang::DiagnosticsEngine::Fatal)
            ++errorCount;
        const char* level = nullptr;
        switch (lv) {
            default: return;
            case clang::DiagnosticsEngine::Note:    level = "note";    break;
            case clang::DiagnosticsEngine::Warning: level = "warning"; break;
            case clang::DiagnosticsEngine::Error:
            case clang::DiagnosticsEngine::Fatal:   level = "error";   break;
        }
        llvm::SmallString<256> msg;
        info.FormatDiagnostic(msg);
        os << level << ": " << msg.c_str() << "\n";
    }
};

// 目标三元组
inline std::string defaultTriple() {
#if LLVM_VERSION_MAJOR >= 15
    return llvm::sys::getDefaultTargetTriple();
#else
    return llvm::sys::getHostTriple();
#endif
}

// 初始化本机目标
inline void initTargets() {
    static bool done = false;
    if (done) return;
    done = true;
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();
}

// ① Clang：.cpp → .o（进程内）
inline bool compileToObject(const std::vector<std::string>& userArgs,
                            const std::string& triple,
                            std::string* err) {
    initTargets();
    auto fs = llvm::vfs::getRealFileSystem();

    std::vector<std::string> args;
    args.push_back(clangBinary());
    args.insert(args.end(), userArgs.begin(), userArgs.end());
#ifdef AE_CLANG_RESOURCE_DIR
    args.push_back("-resource-dir");
    args.push_back(AE_CLANG_RESOURCE_DIR);
#endif

    // LLVM 21 起 DiagnosticOptions 不再是引用计数类型（没有 Retain/Release），
    // 不能塞进 IntrusiveRefCntPtr。改成直接构造 DiagnosticsEngine —— 它接收
    // DiagnosticOptions 的引用，不要求引用计数。这样各版本都能编。
    DiagSaver dc;
    clang::DiagnosticOptions diagOpts;
    llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs> diagIDs(
        new clang::DiagnosticIDs());
    clang::DiagnosticsEngine diags(diagIDs, diagOpts, &dc, false);

    std::vector<const char*> argv;
    for (auto& a : args) argv.push_back(a.c_str());

    clang::driver::Driver drv(clangBinary(), triple, diags, "clang++", fs);
    drv.setCheckInputsExist(false);
    std::unique_ptr<clang::driver::Compilation> comp(drv.BuildCompilation(argv));
    if (!comp || comp->containsError()) {
        if (err) *err = dc.message.empty() ? "clang driver: 参数解析失败" : dc.message;
        return false;
    }
    const auto& jobs = comp->getJobs();
    if (jobs.size() != 1) {
        if (err) *err = "clang driver: 期望 1 个编译任务，实际 " +
                        std::to_string(jobs.size());
        return false;
    }
    const llvm::opt::ArgStringList& ccArgs = jobs.begin()->getArguments();

    std::shared_ptr<clang::CompilerInvocation> invoc(
        new clang::CompilerInvocation());
    clang::CompilerInvocation::CreateFromArgs(*invoc, ccArgs, diags);

    // LLVM 21：没有 setInvocation，createDiagnostics 也要求先给 VFS。
    // 用构造函数直接把 invocation 传进去，再按新版签名建诊断引擎。
#if LLVM_VERSION_MAJOR >= 21
    auto ci = std::make_unique<clang::CompilerInstance>(invoc);
    ci->createDiagnostics(*fs, diagOpts, &dc, false);
#else
    auto ci = std::make_unique<clang::CompilerInstance>();
    ci->setInvocation(std::move(invoc));
    ci->createDiagnostics(&dc, false);
#endif

    // 兜底：某些 LLVM 构建里 createDiagnostics 并不会真的装上引擎，
    // 此时直接 getDiagnostics() 会断言失败（abort）。这里自己造一个塞进去；
    // 实在装不上就干净地返回 false，而不是让整个 aec 崩掉。
    if (!ci->hasDiagnostics()) {
        if constexpr (HasSetDiagnostics<clang::CompilerInstance>::value) {
            // 故意用 new 不 delete：无法确定 CompilerInstance 是否会接管所有权
            auto* de = new clang::DiagnosticsEngine(diagIDs, diagOpts, &dc, false);
            ci->setDiagnostics(de);
        }
    }
    if (!ci->hasDiagnostics()) {
        if (err) *err = "clang: 无法创建诊断引擎（CompilerInstance 没有 diagnostics）";
        return false;
    }
    ci->getDiagnostics().getDiagnosticOptions().ShowCarets = false;
    ci->createFileManager(fs);
    ci->createSourceManager(ci->getFileManager());

    clang::EmitObjAction action;
    bool ok = ci->ExecuteAction(action);
    if (!ok || dc.errorCount > 0) {
        if (err) *err = dc.message.empty() ? "clang: 编译失败" : dc.message;
        return false;
    }
    return true;
}

// ② LLD：.o → 可执行文件（进程内）
inline bool linkExecutable(const std::vector<std::string>& args,
                           const std::string& triple,
                           std::string* err) {
    std::string capErr, capOut;
    llvm::raw_string_ostream errStream(capErr);
    llvm::raw_string_ostream outStream(capOut);

    std::vector<const char*> argv;
    argv.push_back("ld.lld");
    for (auto& a : args) argv.push_back(a.c_str());
    llvm::ArrayRef<const char*> argvRef(argv);

    bool isWin = triple.find("windows") != std::string::npos ||
                 triple.find("win32")   != std::string::npos ||
                 triple.find("msvc")    != std::string::npos;
    bool isMac = triple.find("apple")   != std::string::npos ||
                 triple.find("darwin")  != std::string::npos ||
                 triple.find("macos")   != std::string::npos;

    // exitEarly=false：出错时别 exit 掉我们的进程，交给我们返回 false
    if (isWin)       lld::coff::link(argvRef, outStream, errStream, false, false);
    else if (isMac)  lld::macho::link(argvRef, outStream, errStream, false, false);
    else             lld::elf::link(argvRef, outStream, errStream, false, false);
    errStream.flush();
    if (!capErr.empty()) { if (err) *err = capErr; return false; }
    return true;
}

// 用 Driver 生成完整链接参数（不执行）
// 安卓上必须这么做。LLD 自己不知道：
// sysroot=/data/data/com.termux/files
// dynamic-linker /system/bin/linker64
// .../crtbegin_dynamic.o ... .../crtend_android.o
// 这些只有 Clang 的 Driver 会算出来。
inline bool driverLinkArgs(const std::string& objPath,
                           const std::string& exePath,
                           const std::string& triple,
                           std::vector<std::string>* outArgs,
                           std::string* err) {
    initTargets();
    auto fs = llvm::vfs::getRealFileSystem();

    std::vector<std::string> args;
    args.push_back(clangBinary());
    if (!triple.empty()) { args.push_back("-target"); args.push_back(triple); }
    if (triple.find("android") != std::string::npos ||
        triple.find("apple")   != std::string::npos)
        args.push_back("-stdlib=libc++");
    args.push_back(objPath);
    args.push_back("-o");
    args.push_back(exePath);

    std::vector<const char*> argv;
    for (auto& a : args) argv.push_back(a.c_str());

    DiagSaver dc;
    clang::DiagnosticOptions diagOpts;
    llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs> diagIDs(
        new clang::DiagnosticIDs());
    clang::DiagnosticsEngine diags(diagIDs, diagOpts, &dc, false);

    clang::driver::Driver drv(clangBinary(), triple, diags, "clang++", fs);
    drv.setCheckInputsExist(false);
    std::unique_ptr<clang::driver::Compilation> comp(drv.BuildCompilation(argv));
    if (!comp || comp->containsError()) {
        if (err) *err = dc.message.empty() ? "driver: 链接参数生成失败" : dc.message;
        return false;
    }

    const auto& jobs = comp->getJobs();
    if (jobs.empty()) { if (err) *err = "driver: 没有产生链接任务"; return false; }

    // 最后一个 job 通常是链接；跳过 argv[0]（那是外部 ld 的路径）
    auto it = jobs.begin();
    std::advance(it, jobs.size() - 1);
    const llvm::opt::ArgStringList& ccArgs = it->getArguments();
    outArgs->clear();
    bool first = true;
    for (const char* a : ccArgs) {
        if (first) { first = false; continue; }
        outArgs->push_back(a);
    }
    return true;
}

// ③ 一步到位：.cpp → 可执行文件（全程进程内）
inline Result buildExecutable(const std::string& cppPath,
                              const std::string& exePath,
                              const std::string& includeDir,
                              const std::string& optFlag,
                              const std::string& tripleIn,
                              const std::vector<std::string>& extraFlags,
                              bool keepObject) {
    Result r;
    std::string triple = tripleIn.empty() ? defaultTriple() : tripleIn;
    std::string objPath = exePath + ".o";

    // ① 编译
    std::vector<std::string> cArgs;
    cArgs.push_back("-std=c++17");
    cArgs.push_back(optFlag.empty() ? "-O2" : optFlag);
    cArgs.push_back("-fexceptions");   // Ae 的 try/catch 依赖异常
    cArgs.push_back("-frtti");
    if (!includeDir.empty()) { cArgs.push_back("-I"); cArgs.push_back(includeDir); }
    if (!tripleIn.empty()) { cArgs.push_back("-target"); cArgs.push_back(triple); }
    for (auto& f : extraFlags) cArgs.push_back(f);
    cArgs.push_back("-c");
    cArgs.push_back(cppPath);
    cArgs.push_back("-o");
    cArgs.push_back(objPath);

    std::string cerr;
    r.stage = "compile";
    if (!compileToObject(cArgs, triple, &cerr)) { r.message = cerr; return r; }

    // ② 链接（参数交给 Driver 算，执行交给 LLD）
    std::vector<std::string> lArgs;
    r.stage = "link";
    if (!driverLinkArgs(objPath, exePath, triple, &lArgs, &cerr)) {
        r.message = cerr; return r;
    }
    bool hasStdCxx = false;
    for (auto& a : lArgs)
        if (a.find("c++") != std::string::npos) { hasStdCxx = true; break; }
    if (!hasStdCxx) lArgs.push_back("-lc++");

    std::string lerr;
    if (!linkExecutable(lArgs, triple, &lerr)) { r.message = lerr; return r; }

    if (!keepObject) std::remove(objPath.c_str());
    r.ok = true;
    return r;
}

} // namespace aellvm

#else   // !AE_ENABLE_LLVM —— 空壳，让 aec.cpp 不用写 #ifdef

namespace aellvm {
inline std::string defaultTriple() { return ""; }
inline Result buildExecutable(const std::string&, const std::string&,
                              const std::string&, const std::string&,
                              const std::string&, const std::vector<std::string>&,
                              bool) {
    Result r;
    r.stage = "compile";
    r.message = "未链入 Clang/LLD";
    return r;
}
} // namespace aellvm

#endif // AE_ENABLE_LLVM
#endif // AE_LLVM_H
