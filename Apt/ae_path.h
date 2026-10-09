// =============================================================================
//  ae_path.h —— 路径与文件读写的编码处理（Windows 上必须做的事）
// -----------------------------------------------------------------------------
//  问题
//    工具链内部【一律用 UTF-8】：源码里的字符串（run("b.aeo")、io.readFile）是
//    UTF-8；而 Windows 上
//      · 命令行参数（argv）是本机 ANSI 代码页（中文系统上 GBK）
//      · 文件 API（ifstream/fopen）按 ANSI 解释路径
//    于是「中文文件名」会踩两种坑：从命令行传进来的路径被打上 GBK 字节、
//    脚本里写的路径是 UTF-8 字节 —— 谁都可能打不开对方的文件。
//
//  做法
//    · ansiToUtf8(s)     把 argv 这类本机 ANSI 字节转成 UTF-8
//    · resolvePath(raw)  同一个字节串两种解释都试（UTF-8 优先，再试 ANSI），
//                        返回真实存在的那个的 UTF-8 形式；诊断里显示的文件名才不乱码
//    · readFileBytes / writeFileBytes / fileExists
//                        走宽字符 API（_wfopen / GetFileAttributesW），彻底绕开代码页
//    非 Windows 平台：这些函数退化成直接用 std::ifstream / fopen（那边本来就是 UTF-8）。
//
//  用法：`#include "ae_path.h"`，然后 aepath::xxx
// =============================================================================
#ifndef AE_PATH_H
#define AE_PATH_H

#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

namespace aepath {

// UTF-8 字节串 → 宽字符
inline std::wstring toWide(const std::string& s) {
#ifdef _WIN32
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
#else
    return std::wstring(s.begin(), s.end());
#endif
}

// 宽字符 → UTF-8
inline std::string toUtf8(const std::wstring& w) {
#ifdef _WIN32
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
#else
    return std::string(w.begin(), w.end());
#endif
}

// 本机 ANSI（argv、控制台传入的路径）→ UTF-8
inline std::string ansiToUtf8(const std::string& raw) {
#ifdef _WIN32
    if (raw.empty()) return raw;
    int n = MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), nullptr, 0);
    if (n <= 0) return raw;
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), &w[0], n);
    return toUtf8(w);
#else
    return raw;
#endif
}

// 文件存在？（UTF-8 路径；Windows 走宽字符 API）
inline bool fileExists(const std::string& utf8Path) {
#ifdef _WIN32
    std::wstring w = toWide(utf8Path);
    if (w.empty()) return false;
    DWORD a = GetFileAttributesW(w.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    std::ifstream f(utf8Path, std::ios::binary);
    return (bool)f;
#endif
}

// 把"可能是 UTF-8、也可能是 ANSI"的路径解析成真实存在的【UTF-8 路径】
inline bool resolvePath(const std::string& raw, std::string* utf8Out) {
    if (raw.empty()) return false;
    if (fileExists(raw)) { if (utf8Out) *utf8Out = raw; return true; }
    std::string ansi = ansiToUtf8(raw);
    if (ansi != raw && fileExists(ansi)) { if (utf8Out) *utf8Out = ansi; return true; }
    return false;
}

// 读整个文件（UTF-8 路径）
inline bool readFileBytes(const std::string& utf8Path, std::vector<uint8_t>* out) {
    if (!out) return false;
    out->clear();
#ifdef _WIN32
    std::wstring w = toWide(utf8Path);
    if (w.empty()) return false;
    FILE* f = _wfopen(w.c_str(), L"rb");
#else
    FILE* f = std::fopen(utf8Path.c_str(), "rb");
#endif
    if (!f) return false;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out->insert(out->end(), buf, buf + n);
    std::fclose(f);
    return true;
}

// 读成文本（UTF-8 路径；按字节读，不做换行转换）
inline bool readFileText(const std::string& utf8Path, std::string* out) {
    std::vector<uint8_t> b;
    if (!readFileBytes(utf8Path, &b)) return false;
    if (out) out->assign(b.begin(), b.end());
    return true;
}

// 写整个文件（UTF-8 路径）；失败返回 false
inline bool writeFileBytes(const std::string& utf8Path, const void* data, size_t len) {
#ifdef _WIN32
    std::wstring w = toWide(utf8Path);
    if (w.empty()) return false;
    FILE* f = _wfopen(w.c_str(), L"wb");
#else
    FILE* f = std::fopen(utf8Path.c_str(), "wb");
#endif
    if (!f) return false;
    if (len && std::fwrite(data, 1, len, f) != len) { std::fclose(f); return false; }
    std::fclose(f);
    return true;
}

// 取目录（保留原分隔符风格；没有目录部分就返回空串）
inline std::string dirOf(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return (s == std::string::npos) ? std::string() : p.substr(0, s);
}

// =============================================================================
//  资产目录（assets/）查找
// -----------------------------------------------------------------------------
//  为什么要这个
//    项目结构是 <项目>/apt（放脚本） 与 <项目>/assets（放素材），
//    而运行时的工作目录是 apt —— 于是在脚本里想读 assets 里的素材，
//    只能写 "..\\assets\\地图.tmx" 这种带 ../ 的路径，很啰嗦也容易写错。
//
//  约定（见 resolveAssetPath）
//    · 绝对路径、带 ../ 或子目录的相对路径：先【原样】找（旧行为完全保留）
//    · 找不到的 / 只写了文件名的：再去资产根目录里挨个找
//    · 顺序：AE_ASSETS 环境变量 → 当前目录/assets → 上一级/assets
//          → 上两级/assets → 可执行文件旁边/assets
//
//  写操作（io.writeFile 等）不走这套：凭空造一个输出位置比报错更危险。
// =============================================================================

// 拼路径：左边没有结尾分隔符时补一个（跟着左边原有的风格）
inline std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '/' || a.back() == '\\') return a + b;
#ifdef _WIN32
    return a + "\\" + b;
#else
    return a + "/" + b;
#endif
}

// 绝对路径？（含盘符、或以分隔符开头）——这种不再去资产目录里绕
inline bool isAbsolutePath(const std::string& p) {
    if (p.empty()) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
#ifdef _WIN32
    return p.size() >= 2 && p[1] == ':';
#else
    return false;
#endif
}

// 资产根目录列表（按优先级）
inline std::vector<std::string> assetRoots() {
    std::vector<std::string> out;

    const char* env = std::getenv("AE_ASSETS");         // 想显式指定就用它
    if (env && *env) {
        const char sep =
#ifdef _WIN32
            ';';
#else
            ':';
#endif
        std::string rest(env);
        size_t pos = 0;
        while (pos <= rest.size()) {
            size_t next = rest.find(sep, pos);
            std::string one = (next == std::string::npos) ? rest.substr(pos) : rest.substr(pos, next - pos);
            if (!one.empty()) out.push_back(one);
            if (next == std::string::npos) break;
            pos = next + 1;
        }
    }

    // 相对当前工作目录：apt 里跑就命中 ../assets，在根目录跑就命中 ./assets
    out.push_back("assets");
    out.push_back("../assets");
    out.push_back("../../assets");

#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);       // 可执行文件旁边
    if (n > 0 && n < MAX_PATH) {
        std::string dir = dirOf(toUtf8(std::wstring(buf, n)));
        if (!dir.empty()) out.push_back(joinPath(dir, "assets"));
    }
#endif
    return out;
}

// 按上面的约定找文件：命中返回 true 并把真实存在的 UTF-8 路径写进 utf8Out
inline bool resolveAssetPath(const std::string& raw, std::string* utf8Out) {
    if (raw.empty()) return false;
    if (resolvePath(raw, utf8Out)) return true;                 // ① 原样优先，旧写法照样能用
    if (isAbsolutePath(raw)) return false;                      // 绝对路径没找到就别绕了
    for (const std::string& root : assetRoots()) {              // ② 再去资产目录里找
        if (resolvePath(joinPath(root, raw), utf8Out)) return true;
    }
    return false;
}

}  // namespace aepath

#endif  // AE_PATH_H
