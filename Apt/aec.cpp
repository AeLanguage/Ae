// Ae 编译器 (aec) —— 把 .ae 源码编译成字节码，交给 ae 执行
// 编译流水线（前端已拆成头文件，供字节码后端与将来的 AOT 后端共用）：
// ae_lexer.h 源码 → Token 流（含字符串插值展开）
// ae_parser.h Token → AST
// ae_ast.h AST 节点定义（两个后端唯一的公共契约）
// ae_emitbc.h AST → 字节码
//
// 本文件只做驱动：读源文件、跑流水线、写 .aeo、渲染诊断。
// 历史沿革见 README.MD（2.0.1b 起改用 AST 后端，2.1.0 起支持 AOT）。

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
#include <memory>
#include "ae_libhost.h"      // 原生库加载（aec 只用它读导出表做编译期检查）
#include "ae_builtin.h"      // 内置库（io / os / path 的导出表在编译时就已知）
#include "ae_diag.h"         // 诊断系统（错误码 + 源码摘录 + 波浪线）
#include "ae_console.h"     // 控制台中文：绕开代码页
#include "ae_path.h"        // 路径编码：UTF-8 / ANSI / 宽字符
// 编译前端（ 已从 aec.cpp 抽出，供字节码后端与将来的 AOT 后端共用）
#include "ae_common.h"       // 诊断消息打包 / UTF-8 / 字节码写入
#include "ae_constpool.h"    // 常量池 + 字节码缓冲区
#include "ae_symtab.h"       // 符号表（词法作用域 / 上值 / 函数签名）
#include "ae_lexer.h"        // 词法分析（Token / Lexer）
#include "ae_diag_render.h"  // 诊断渲染（错误码分类表）
#include "ae_emitbc.h"      // AST → 字节码后端
#include "ae_emitcpp.h"     // AST → C++ 源码（AOT 后端）
#include "ae_llvm.h"        // Clang + LLD 作为库：进程内把 .cpp 编成可执行文件

// 编译器

// 小工具：aec 自己所在的目录
// 生成的 C++ 里是 #include "ae.cpp"，所以 -I 要指向工具链目录（Ae/Apt/）。
static string exeDirOf(const char* argv0) {
    string a = aepath::ansiToUtf8(argv0);
    size_t p = a.find_last_of("/\\");
    if (p == string::npos) return ".";
    return a.substr(0, p);
}

// 外部 C++ 编译器探测（只在没链入 Clang/LLD 时才用到）
static bool findExternalCC(string* out) {
    if (const char* e = getenv("AE_CC")) { if (*e) { *out = e; return true; } }
    const char* names[] = {"g++", "clang++", "c++", "zig"};
    for (auto n : names) {
        string cmd = string("command -v ") + n + " >/dev/null 2>&1";
        if (system(cmd.c_str()) == 0) { *out = n; return true; }
    }
    return false;
}

// main
int main(int argc, char** argv) {
    aecon::install();    // 第一件事：把 cout/cerr/cin 接到控制台（GBK 的 cmd 里中文不乱码）
    string full;            // 源码文本（诊断时要摘录出错那一行）
    vector<string> c_files; // 编译期的源文件表（含 .m 模块）
    string inPath, outPath;
    bool quiet = false;   // -q：编程式调用时不打印成功提示
    bool wantStats = false;
    bool wantAot = false;      // --aot：生成 C++ 源码（不调用 C++ 编译器）
    bool wantAotExe = false;   // --aot-exe：生成 C++ 并直接编出可执行文件
    bool keepObj = false;      // --keep-obj：保留中间 .o（调试用）
    string ccCmd;              // --cc=<命令>：指定外部 C++ 编译器
    string optFlag = "-O2";    // -O<0..3>：优化级别
    string aotTarget;          // --target=<三元组>：交叉编译（需要 LLD）
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            // Windows 上 argv 是本机 ANSI（GBK），工具链内部一律 UTF-8：先转一次
            string a = aepath::ansiToUtf8(argv[i]);
            if (a == "-h" || a == "--help") {          // 加：以前 aec -h 会被当成源文件名
                cout << "Ae 编译器 (aec) 2.1.0\n"
                        "用法: aec <源文件.ae> [-o 输出.aeo] [-q]\n"
                        "  -o <文件>        指定输出的字节码文件名（默认同名 .aeo）\n"
                        "  -q, --quiet      成功时不打印提示（只出错才输出）\n"
                        "  --aot            生成 C++ 源码（AOT），不调用 C++ 编译器\n"
                        "  --aot-exe        生成 C++ 并直接编出可执行文件（优先用内嵌的 Clang+LLD）\n"
                        "  --cc=<命令>      外部 C++ 编译器（没内嵌 Clang 时的回退）\n"
                        "  --target=<三元组> 交叉编译目标，如 x86_64-windows-gnu（需内嵌 LLD）\n"
                        "  -O<0..3>         优化级别（默认 -O2）\n"
                        "  --keep-obj       保留中间的 .o 文件（调试用）\n"
                        "  -h, --help       显示本帮助\n"
                        "输出字节码格式: AeBc 1.8\n"
                        "退出码: 0 成功  1 编译失败\n"
                        "提示: 用法里第二个位置参数也可以直接写输出文件（aec a.ae b.aeo）\n";
                return 0;
            }
            if (a == "-q" || a == "--quiet") {
                quiet = true;
            } else if (a == "--stats") {
                wantStats = true;
            } else if (a == "--aot") {
                wantAot = true;
            } else if (a == "--aot-exe") {
                wantAotExe = true;
            } else if (a == "--keep-obj") {
                keepObj = true;
            } else if (a.rfind("--cc=", 0) == 0) {
                ccCmd = a.substr(5);
            } else if (a.rfind("--target=", 0) == 0) {
                aotTarget = a.substr(9);
            } else if (a.size() == 3 && a[0] == '-' && a[1] == 'O' &&
                       a[2] >= '0' && a[2] <= '3') {
                optFlag = a;
            } else if (a == "-o" && i + 1 < argc) {
                outPath = aepath::ansiToUtf8(argv[++i]);
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
        {   // 源文件读取与 argv 都按 UTF-8 处理：中文文件名、中文模块名都能用
            string real = inPath;
            if (!aepath::resolvePath(inPath, &real)) {
                aediag::printBrief(AE_E_FILE, "无法打开源文件: " + inPath);
                return 1;
            }
            inPath = real;                       // 之后诊断里显示的是 UTF-8 形式，不会乱码
            if (!aepath::readFileText(inPath, &full)) {
                aediag::printBrief(AE_E_FILE, "无法打开源文件: " + inPath);
                return 1;
            }
        }

        {
            // 编译流水线：Lexer → Parser(AST) → Emitter(字节码)
            aeparse::Parser P(full, inPath, 0);
            aeast::Program prog = P.parseProgram();
            aeemit::Emitter E(inPath);
            E.emit(prog);
            if (outPath.empty()) {
                outPath = inPath;
                if (outPath.size() > 3 && outPath.substr(outPath.size()-3) == ".ae")
                    outPath = outPath.substr(0, outPath.size()-3) + ".aeo";
                else outPath += ".aeo";
            }
            if (wantAot || wantAotExe) {
                std::string src;
                aecpp::emitProgram(prog, inPath, src);
                std::string outCpp = outPath;
                if (outCpp.size() > 4 && outCpp.substr(outCpp.size()-4) == ".aeo")
                    outCpp = outCpp.substr(0, outCpp.size()-4) + ".cpp";
                else if (outCpp.size() < 4 || outCpp.substr(outCpp.size()-4) != ".cpp")
                    outCpp += ".cpp";
                { FILE* f = fopen(outCpp.c_str(), "wb");
                  if (!f) { aediag::printBrief(AE_E_FILE, "无法写: " + outCpp); return 1; }
                  fwrite(src.data(), 1, src.size(), f); fclose(f); }

                if (!wantAotExe) {
                    // aot：只生成源码，不调用任何 C++ 编译器（永远可用）
                    if (!quiet) aediag::printOk("AOT 已生成 C++ 源码: " + outCpp);
                    return 0;
                }

                // aot-exe：把 .cpp 编成可执行文件
                string exePath = outCpp.substr(0, outCpp.size() - 4);
                if (aotTarget.find("windows") != string::npos ||
                    aotTarget.find("win32")   != string::npos) exePath += ".exe";

                // ① 优先：内嵌的 Clang + LLD（进程内，零外部进程 → 安卓也能用）
                if (aellvm::available()) {
                    string triple = aotTarget.empty() ? aellvm::defaultTriple() : aotTarget;
                    auto r = aellvm::buildExecutable(outCpp, exePath,
                                                     exeDirOf(argv[0]), optFlag,
                                                     triple, {}, keepObj);
                    if (!r.ok) {
                        aediag::printBrief(AE_E_USAGE,
                            string("AOT ") + (r.stage == "compile" ? "编译" : "链接") +
                            "失败:\n" + r.message);
                        remove(outCpp.c_str());
                        return 1;
                    }
                    if (!quiet) aediag::printOk("AOT 已生成可执行文件: " + exePath);
                    return 0;
                }

                // ② 回退：外部 C++ 编译器（桌面平台可用，安卓上大概率不行）
                string cc = ccCmd;
                if (cc.empty()) findExternalCC(&cc);
                if (cc.empty()) {
                    aediag::printBrief(AE_E_USAGE,
                        string("AOT: 没有可用的 C++ 编译器\n") + aellvm::unavailableReason());
                    if (!quiet) aediag::printOk("已保留 C++ 源码: " + outCpp +
                        "（可用 g++/clang++ 手工编译）");
                    return 1;
                }
                string ic = "-I\"" + exeDirOf(argv[0]) + "\"";
                string cmd = cc + " -std=c++17 " + optFlag + " " + ic +
                             " \"" + outCpp + "\" -o \"" + exePath + "\"";
                if (system(cmd.c_str()) != 0) {
                    aediag::printBrief(AE_E_USAGE, "AOT: C++ 编译器调用失败: " + cc);
                    if (!quiet) aediag::printOk("已保留 C++ 源码: " + outCpp);
                    return 1;
                }
                if (!keepObj) remove(outCpp.c_str());
                if (!quiet) aediag::printOk("AOT 已生成可执行文件: " + exePath);
                return 0;
            }
            aeemit::emitbcSave(E, outPath, inPath);
            c_files = E.fileNames;
            if (wantStats) E.dumpStats();
        }
        if (outPath.empty()) {
            outPath = inPath;
            if (outPath.size() > 3 && outPath.substr(outPath.size() - 3) == ".ae")
                outPath = outPath.substr(0, outPath.size() - 3) + ".aeo";
            else outPath += ".aeo";
        }
    } catch (const exception& e) {
        renderDiag(e.what(), inPath, full, &c_files);
        return 1;
    }
    c_files.clear();
    if (!quiet) {
        vector<uint8_t> chk;
        aepath::readFileBytes(outPath, &chk);
        aediag::printOk("编译成功: " + outPath + "（" + to_string(chk.size()) + " 字节）");
    }
    return 0;
}
