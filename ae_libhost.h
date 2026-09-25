// =============================================================================
//  Ae 原生库宿主侧加载器  ——  ae_libhost.h
//  aec（编译器）与 ae（虚拟机）共用：编译器只需要 readExports() 拿名字做
//  编译期检查；虚拟机需要 load() 真正调用。
// =============================================================================
#ifndef AE_LIBHOST_H
#define AE_LIBHOST_H

#ifndef _CRT_SECURE_NO_WARNINGS
  #define _CRT_SECURE_NO_WARNINGS      // 屏蔽 getenv 等 MSVC 弃用告警
#endif

#include "ae_native.h"
#include <string>
#include <vector>
#include <cstdlib>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX     // windows.h 的 max/min 宏会破坏 std::max(...)
  #endif
  #include <windows.h>
#else
  #include <dlfcn.h>
  #include <unistd.h>
#endif

namespace aehost {

// 找到宿主可执行文件所在目录（用于定位 <exe>/lib）
inline std::string exeDir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf, n);
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string p = (n > 0) ? std::string(buf, n) : std::string();
#endif
    size_t s = p.find_last_of("/\\");
    return (s == std::string::npos) ? std::string(".") : p.substr(0, s);
}

inline std::string dirOf(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    return (s == std::string::npos) ? std::string(".") : path.substr(0, s);
}

// 依次尝试的目录：<exe>/lib → <exe> → <script目录>/lib → <script目录> → cwd/lib → cwd → AE_LIB_PATH
inline std::vector<std::string> searchDirs(const std::string& hintDir) {
    std::vector<std::string> dirs;
    std::string ex = exeDir();
    dirs.push_back(ex + "/lib");
    dirs.push_back(ex);
    if (!hintDir.empty()) {
        dirs.push_back(hintDir + "/lib");
        dirs.push_back(hintDir);
    }
    dirs.push_back("lib");
    dirs.push_back(".");
    if (const char* env = getenv("AE_LIB_PATH")) {
        std::string s = env, cur;
        for (char c : s) {
            if (c == ';' || c == ':') { if (!cur.empty()) dirs.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) dirs.push_back(cur);
    }
    return dirs;
}

// 候选文件名：<name>.dll / lib<name>.dll（Windows）、lib<name>.so / <name>.so（其它）
inline std::vector<std::string> candidateNames(const std::string& name) {
#ifdef _WIN32
    return { name + ".dll", "lib" + name + ".dll", name };
#else
    return { "lib" + name + ".so", name + ".so", name };
#endif
}

struct LoadedModule {
    void*      handle = nullptr;
    const AeModule* mod = nullptr;
    std::string path;
};

// 加载一个库；失败时 outTried 里是尝试过的完整路径列表
inline bool loadLibrary(const std::string& name, const std::string& hintDir,
                        LoadedModule& out, std::vector<std::string>& outTried) {
    for (const std::string& d : searchDirs(hintDir)) {
        for (const std::string& fn : candidateNames(name)) {
            std::string full = d + "/" + fn;
#ifdef _WIN32
            HMODULE h = LoadLibraryA(full.c_str());
            if (!h) continue;
            const AeModule* m = (const AeModule*)GetProcAddress(h, "ae_module");
#else
            void* h = dlopen(full.c_str(), RTLD_NOW);
            if (!h) continue;
            const AeModule* m = (const AeModule*)dlsym(h, "ae_module");
#endif
            outTried.push_back(full);
            if (!m) {                       // 加载到了但没有 ae_module 导出
#ifdef _WIN32
                FreeLibrary(h);
#else
                dlclose(h);
#endif
                return false;
            }
            out.handle = h;
            out.mod    = m;
            out.path   = full;
            return true;
        }
        outTried.push_back(d + "/" + candidateNames(name)[0]);
    }
    return false;
}

// 只读导出表（编译器用）
inline const AeNativeDef* readExports(const LoadedModule& lm, int& count) {
    count = 0;
    if (!lm.mod || !lm.mod->getExports) return nullptr;
    return lm.mod->getExports(&count);
}

// 拼「找过哪些路径」的报错信息
inline std::string triedText(const std::string& name, const std::vector<std::string>& tried) {
    std::string s = "找不到库 '" + name + "'（已尝试：";
    for (size_t i = 0; i < tried.size(); i++) {
        if (i) s += ", ";
        s += tried[i];
    }
    s += "）";
    return s;
}

}  // namespace aehost

#endif  // AE_LIBHOST_H
