// =============================================================================
//  ae_libutil.h —— 原生库共用的小工具（参数检查 / lastError / 平台转换）
// -----------------------------------------------------------------------------
//  每个库都是独立 DLL，所以这些 inline 函数各有一份副本、状态也各自独立
//  （这正是我们想要的：io.lastError() 与 os.lastError() 互不干扰）。
//
//  约定（所有库统一）：
//    · 预期失败 → aelib::fail("原因") + 返回 null/false，由 <库>.lastError() 查询
//    · 程序性错误（参数类型/个数）→ aelib::raise(vm, "原因")，立刻成为运行错误
// =============================================================================
#ifndef AE_LIBUTIL_H
#define AE_LIBUTIL_H

#ifdef _WIN32
  #ifndef _CRT_SECURE_NO_WARNINGS
    #define _CRT_SECURE_NO_WARNINGS
  #endif
#endif

#include "../ae_native.h"
#include <string>
#include <vector>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <unistd.h>
  #include <limits.h>
  #include <cstring>
  #include <cerrno>
#endif

namespace aelib {

// 每个库自己的 lastError 存储
inline std::string& errorSlot() { static std::string s; return s; }
inline void fail(const std::string& why) { errorSlot() = why; }
inline void clearError() { errorSlot().clear(); }

inline void raise(AeVM* vm, const std::string& msg) { ae_raise(vm, msg); }

// 参数检查：类型不对直接 raise（返回空值让编译器安心）
inline const std::string& argStr(AeVM* vm, AeValue* argv, int argc, int i, const char* fn) {
    static const std::string kEmpty;
    if (i >= argc || !ae_is_str(argv[i])) {
        raise(vm, std::string(fn) + " 的第 " + std::to_string(i + 1) + " 个参数需要 string，得到 " +
                   (i < argc ? (ae_is_int(argv[i]) ? "int" : ae_is_float(argv[i]) ? "float" :
                                ae_is_bool(argv[i]) ? "bool" : ae_is_table(argv[i]) ? "table" : "null")
                             : "缺失"));
        return kEmpty;
    }
    return *(const std::string*)argv[i].p;
}
inline int64_t argInt(AeVM* vm, AeValue* argv, int argc, int i, const char* fn) {
    if (i >= argc || !ae_is_int(argv[i])) {
        raise(vm, std::string(fn) + " 的第 " + std::to_string(i + 1) + " 个参数需要 int");
        return 0;
    }
    return argv[i].i;
}
inline std::string argOptStr(AeVM* vm, AeValue* argv, int argc, int i) {
    if (i >= argc) return std::string();
    const std::string* s = (const std::string*)argv[i].p;
    return s ? *s : std::string();
}
inline void wantArgs(AeVM* vm, size_t got, size_t need, const char* fn) {
    if (got != need)
        raise(vm, std::string(fn) + " 需要 " + std::to_string(need) + " 个参数，得到 " + std::to_string(got));
}

// ── 平台文本转换 ──
#ifdef _WIN32
inline std::string utf16ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
inline std::wstring utf8ToUtf16(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
inline std::string sysErr() {
    DWORD e = GetLastError();
    if (!e) return "系统调用失败";
    LPSTR buf = nullptr;
    DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, (LPSTR)&buf, 0, nullptr);
    std::string m = (n && buf) ? std::string(buf, n) : ("错误码 " + std::to_string(e));
    if (buf) LocalFree(buf);
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r' || m.back() == ' ')) m.pop_back();
    return m;
}
#else
inline std::string sysErr() { return std::strerror(errno); }
#endif

// 当前工作目录（UTF-8）
inline std::string currentDir() {
#ifdef _WIN32
    DWORD need = GetCurrentDirectoryW(0, nullptr);
    std::wstring buf(need, L'\0');
    DWORD got = GetCurrentDirectoryW(need, &buf[0]);
    buf.resize(got);
    return utf16ToUtf8(buf);
#else
    char buf[PATH_MAX];
    if (!getcwd(buf, sizeof(buf))) return std::string();
    return std::string(buf);
#endif
}

}  // namespace aelib

#endif  // AE_LIBUTIL_H
