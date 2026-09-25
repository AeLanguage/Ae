// =============================================================================
//  path —— Ae 的路径处理库（纯字符串运算，不碰文件系统）
// -----------------------------------------------------------------------------
//  ★ 核心设计：本库用 "/" 作为【规范分隔符】，所有函数都接受 "/" 和 "\"，
//    返回的路径一律用 "/"。原因：
//      · Windows 的文件 API 本来就接受 "/"，所以结果可直接喂给 io/os
//      · 输出与平台无关 → 测试用例的黄金输出可以跨平台一致
//      · 避免 "C:\Temp" 和 "C:/Temp" 两种写法在字符串比较里互相打架
//    需要显示成原生反斜杠时用 path.toNative()。
//
//  失败处理与其他库一致：预期失败 → null + path.lastError()；
//  但本库几乎全是纯字符串运算，只有 abs() 需要读当前目录，实际很少失败。
//
//  编译（在 Apt/lib 目录下）：
//      clang++ -std=c++17 -O2 -shared path.cpp -o path.dll
//
//  脚本侧：
//      imp path
//      local full = path.join(os.tempDir(), "sub", "a.txt")   // "C:/Users/.../Temp/sub/a.txt"
//      prln(path.dirname(full), path.basename(full), path.ext(full))
// =============================================================================
#ifdef _WIN32
  #define _CRT_SECURE_NO_WARNINGS
#endif

#include "ae_libutil.h"
#include <cctype>

using namespace aelib;

// ---------------------------------------------------------------------------
//  规范形式与内部工具
// ---------------------------------------------------------------------------
static std::string canonSep(const std::string& p) {
    std::string s = p;
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}
static bool hasDrive(const std::string& s) {   // "C:" / "C:/..."
    return s.size() >= 2 && std::isalpha((unsigned char)s[0]) && s[1] == ':';
}
static bool isAbsCanon(const std::string& s) {
    if (s.empty()) return false;
    if (s[0] == '/') return true;              // 含 UNC 的 "//srv/share"
    return hasDrive(s);                        // 盘符形式一律视为绝对（跨平台一致）
}

// 折叠 "." / ".." / 重复分隔符；保留 UNC 与盘符前缀
static std::string normalizePath(const std::string& in) {
    std::string s = canonSep(in);
    if (s.empty()) return ".";
    std::string prefix;
    size_t i = 0;
    if (s.size() >= 2 && s[0] == '/' && s[1] == '/') { prefix = "//"; i = 2; }
    else if (s[0] == '/')                            { prefix = "/";  i = 1; }
    else if (hasDrive(s)) {
        prefix = s.substr(0, 2);
        i = 2;
        if (i < s.size() && s[i] == '/') { prefix += "/"; i++; }
    }
    std::vector<std::string> parts;
    std::string cur;
    for (size_t k = i; k <= s.size(); k++) {
        if (k == s.size() || s[k] == '/') {
            if (!cur.empty()) {
                if (cur == ".") { /* 丢掉 */ }
                else if (cur == "..") {
                    if (!parts.empty() && parts.back() != "..") parts.pop_back();
                    else if (prefix.empty()) parts.push_back("..");   // 相对路径保留 ..
                    // 绝对路径下越过根 → 丢弃
                } else parts.push_back(cur);
                cur.clear();
            }
        } else cur += s[k];
    }
    std::string out = prefix;
    for (size_t k = 0; k < parts.size(); k++) {
        if (k) out += "/";
        out += parts[k];
    }
    return out.empty() ? "." : out;
}

static std::string dropTrailingSep(const std::string& p) {
    std::string d = canonSep(p);
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    return d;
}
static std::string baseOf(const std::string& p) {
    std::string d = dropTrailingSep(p);
    size_t pos = d.find_last_of('/');
    return (pos == std::string::npos) ? d : d.substr(pos + 1);
}
static std::string extOf(const std::string& p) {
    std::string b = baseOf(p);
    size_t dot = b.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return "";   // ".gitignore" 视为无扩展名
    return b.substr(dot);
}

// ---------------------------------------------------------------------------
//  查询
// ---------------------------------------------------------------------------
static int nat_sep(AeVM* vm, int, AeValue*) {
#ifdef _WIN32
    ae_push_str(vm, "\\");
#else
    ae_push_str(vm, "/");
#endif
    return 1;
}
static int nat_isAbsolute(AeVM* vm, int argc, AeValue* argv) {
    ae_push_bool(vm, isAbsCanon(canonSep(argStr(vm, argv, argc, 0, "isAbsolute"))) ? 1 : 0);
    return 1;
}

// ---------------------------------------------------------------------------
//  拆解
// ---------------------------------------------------------------------------
static int nat_dirname(AeVM* vm, int argc, AeValue* argv) {
    std::string d = dropTrailingSep(argStr(vm, argv, argc, 0, "dirname"));
    size_t pos = d.find_last_of('/');
    if (pos == std::string::npos) { ae_push_str(vm, ""); return 1; }   // "a.txt" → ""
    if (pos == 0)                 { ae_push_str(vm, "/"); return 1; }  // "/a" → "/"
    if (pos == 2 && hasDrive(d))  { ae_push_str(vm, d.substr(0, 3)); return 1; }  // "C:/a" → "C:/"
    ae_push_str(vm, d.substr(0, pos));
    return 1;
}
static int nat_basename(AeVM* vm, int argc, AeValue* argv) {
    ae_push_str(vm, baseOf(argStr(vm, argv, argc, 0, "basename")));
    return 1;
}
static int nat_stem(AeVM* vm, int argc, AeValue* argv) {
    const std::string& p = argStr(vm, argv, argc, 0, "stem");
    std::string b = baseOf(p), e = extOf(p);
    ae_push_str(vm, e.empty() ? b : b.substr(0, b.size() - e.size()));
    return 1;
}
static int nat_ext(AeVM* vm, int argc, AeValue* argv) {
    ae_push_str(vm, extOf(argStr(vm, argv, argc, 0, "ext")));
    return 1;
}
static int nat_split(AeVM* vm, int argc, AeValue* argv) {
    std::string s = canonSep(argStr(vm, argv, argc, 0, "split"));
    AeValue t = ae_new_table(vm);
    int64_t idx = 0;
    std::string cur;
    for (size_t k = 0; k <= s.size(); k++) {
        if (k == s.size() || s[k] == '/') {
            if (!cur.empty()) { ae_table_set(vm, t, ae_int_value(++idx), ae_str_value(vm, cur)); cur.clear(); }
        } else cur += s[k];
    }
    ae_push_table(vm, t);
    return 1;
}

// ---------------------------------------------------------------------------
//  组装与规范化
// ---------------------------------------------------------------------------
static int nat_join(AeVM* vm, int argc, AeValue* argv) {
    std::string acc;
    for (int i = 0; i < argc; i++) {
        const std::string& p = argStr(vm, argv, argc, i, "join");
        if (p.empty()) continue;
        std::string c = canonSep(p);
        if (acc.empty())        acc = c;
        else if (isAbsCanon(c)) acc = c;          // 后面出现绝对路径 → 丢弃前面的（同 Python）
        else { acc += "/"; acc += c; }
    }
    ae_push_str(vm, normalizePath(acc));
    return 1;
}
static int nat_normalize(AeVM* vm, int argc, AeValue* argv) {
    ae_push_str(vm, normalizePath(argStr(vm, argv, argc, 0, "normalize")));
    return 1;
}
static int nat_abs(AeVM* vm, int argc, AeValue* argv) {
    const std::string& p = argStr(vm, argv, argc, 0, "abs");
    std::string c = canonSep(p);
    if (isAbsCanon(c)) { ae_push_str(vm, normalizePath(c)); return 1; }
    std::string cwd = currentDir();
    if (cwd.empty()) {
        fail("无法读取当前工作目录: " + sysErr());
        ae_push_null(vm);
        return 1;
    }
    ae_push_str(vm, normalizePath(canonSep(cwd) + "/" + c));
    return 1;
}
static int nat_toNative(AeVM* vm, int argc, AeValue* argv) {
    std::string s = argStr(vm, argv, argc, 0, "toNative");
#ifdef _WIN32
    for (char& c : s) if (c == '/') c = '\\';
#endif
    ae_push_str(vm, s);
    return 1;
}
static int nat_replaceExt(AeVM* vm, int argc, AeValue* argv) {
    const std::string& p = argStr(vm, argv, argc, 0, "replaceExt");
    std::string ne = argStr(vm, argv, argc, 1, "replaceExt");
    std::string d = canonSep(p);
    size_t pos = d.find_last_of('/');
    std::string dir  = (pos == std::string::npos) ? std::string() : d.substr(0, pos + 1);
    std::string base = (pos == std::string::npos) ? d : d.substr(pos + 1);
    std::string e  = extOf(base);
    std::string st = e.empty() ? base : base.substr(0, base.size() - e.size());
    if (!ne.empty() && ne[0] != '.') ne = "." + ne;      // 允许不写点
    ae_push_str(vm, dir + st + ne);
    return 1;
}

static int nat_lastError(AeVM* vm, int, AeValue*) {
    ae_push_str(vm, errorSlot());
    return 1;
}

// ---------------------------------------------------------------------------
//  导出表
// ---------------------------------------------------------------------------
static const AeNativeDef kExports[] = {
    // 查询
    { "sep",          0, 0, nat_sep        },
    { "isAbsolute",   1, 1, nat_isAbsolute },
    // 拆解
    { "dirname",      1, 1, nat_dirname    },
    { "basename",     1, 1, nat_basename   },
    { "stem",         1, 1, nat_stem       },
    { "ext",          1, 1, nat_ext        },
    { "split",        1, 1, nat_split      },
    // 组装与规范化
    { "join",         1, 255, nat_join     },
    { "normalize",    1, 1, nat_normalize  },
    { "abs",          1, 1, nat_abs        },
    { "toNative",     1, 1, nat_toNative   },
    { "replaceExt",   2, 2, nat_replaceExt },
    // 错误查询
    { "lastError",    0, 0, nat_lastError  },
};

static const AeNativeDef* path_getExports(int* count) {
    *count = (int)(sizeof(kExports) / sizeof(kExports[0]));
    return kExports;
}

AE_MODULE("path", path_getExports)
