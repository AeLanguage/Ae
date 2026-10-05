// =============================================================================
//  os —— Ae 的系统信息库
// -----------------------------------------------------------------------------
//  覆盖脚本最常用的"环境"能力：命令行参数、环境变量、工作目录、时间、随机、
//  系统信息。**不含**执行外部进程（那属于 proc 库，涉及安全，单独再议）。
//
//  设计约定（与 io 保持一致）：
//    · 预期失败（环境变量没设、目录切不过去）→ 返回 null/false + os.lastError()
//    · 程序性错误（参数类型错、参数个数不对）→ ae_raise，变成带调用栈的运行错误
//    · 所有字符串都是 UTF-8（Windows 下内部转 UTF-16，中文路径/变量值可用）
//
//  编译（在 Apt/lib 目录下）：
//      clang++ -std=c++17 -O2 -shared os.cpp -o os.dll
//
//  脚本侧：
//      imp os
//      prln(os.args())                       // 命令行参数（.aeo 之后的都算）
//      prln(os.getEnv("PATH") != null)
//      prln(os.formatTime("%Y-%m-%d %H:%M:%S"))
//      os.seed(os.time())
//      prln(os.random(100))                  // 0..99
//      prln("平台:", os.platform(), "CPU:", os.cpuCount())
// =============================================================================
#ifdef _WIN32
  #define _CRT_SECURE_NO_WARNINGS
#endif
#include "../ae_native.h"
#include <string>
#include <vector>
#include <chrono>
#include <random>
#include <thread>
#include <cstdio>
#include <cstring>
#include <ctime>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <unistd.h>
  #include <limits.h>
  extern char** environ;
#endif

static std::string g_lastError;

static std::string sysErr() {
#ifdef _WIN32
    DWORD e = GetLastError();
    if (!e) return "系统调用失败";
    // ★ 修：FormatMessageW + 转 UTF-8（用 A 版拿到的是 GBK，存进 lastError 会变乱码）
    LPWSTR wbuf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, (LPWSTR)&wbuf, 0, nullptr);
    std::string m;
    if (n && wbuf) {
        int bytes = WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)n, nullptr, 0, nullptr, nullptr);
        if (bytes > 0) {
            m.resize((size_t)bytes);
            WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)n, &m[0], bytes, nullptr, nullptr);
        }
        LocalFree(wbuf);
    }
    if (m.empty()) m = "错误码 " + std::to_string(e);
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r' || m.back() == ' ')) m.pop_back();
    return m;
#else
    return std::strerror(errno);
#endif
}

#ifdef _WIN32
static std::string utf16ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring utf8ToUtf16(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
#endif

// 参数检查
static const std::string& argStr(AeVM* vm, AeValue* argv, int argc, int i, const char* fn) {
    static const std::string kEmpty;
    if (i >= argc || !ae_is_str(argv[i])) {
        ae_raise(vm, std::string(fn) + " 的第 " + std::to_string(i + 1) + " 个参数需要 string");
        return kEmpty;
    }
    return *(const std::string*)argv[i].p;
}
static int64_t argInt(AeVM* vm, AeValue* argv, int argc, int i, const char* fn) {
    if (i >= argc || !ae_is_int(argv[i])) {
        ae_raise(vm, std::string(fn) + " 的第 " + std::to_string(i + 1) + " 个参数需要 int");
        return 0;
    }
    return argv[i].i;
}
static std::string argOptStr(AeVM* vm, AeValue* argv, int argc, int i) {
    if (i >= argc) return std::string();
    return *(const std::string*)argv[i].p;
}

// ---------------------------------------------------------------------------
//  命令行参数
// ---------------------------------------------------------------------------
static int nat_args(AeVM* vm, int, AeValue*) {
    int n = ae_arg_count(vm);
    AeValue t = ae_new_table(vm);
    for (int i = 0; i < n; i++)
        ae_table_set(vm, t, ae_int_value(i + 1), ae_str_value(vm, ae_arg(vm, i)));
    ae_push_table(vm, t);
    return 1;
}
static int nat_scriptPath(AeVM* vm, int, AeValue*) {
    ae_push_str(vm, ae_script_path(vm));
    return 1;
}
static int nat_argCount(AeVM* vm, int, AeValue*) {
    ae_push_int(vm, ae_arg_count(vm));
    return 1;
}

// ---------------------------------------------------------------------------
//  环境变量
// ---------------------------------------------------------------------------
static int nat_getEnv(AeVM* vm, int argc, AeValue* argv) {
    const std::string& name = argStr(vm, argv, argc, 0, "getEnv");
#ifdef _WIN32
    DWORD need = GetEnvironmentVariableW(utf8ToUtf16(name).c_str(), nullptr, 0);
    if (need == 0) { g_lastError = "环境变量未设置: " + name; ae_push_null(vm); return 1; }
    std::wstring buf(need, L'\0');
    DWORD got = GetEnvironmentVariableW(utf8ToUtf16(name).c_str(), &buf[0], need);
    buf.resize(got);
    ae_push_str(vm, utf16ToUtf8(buf));
#else
    const char* v = getenv(name.c_str());
    if (!v) { g_lastError = "环境变量未设置: " + name; ae_push_null(vm); return 1; }
    ae_push_str(vm, v);
#endif
    return 1;
}
static int nat_setEnv(AeVM* vm, int argc, AeValue* argv) {
    const std::string& name = argStr(vm, argv, argc, 0, "setEnv");
    const std::string& val  = argStr(vm, argv, argc, 1, "setEnv");
    bool ok;
#ifdef _WIN32
    ok = SetEnvironmentVariableW(utf8ToUtf16(name).c_str(), utf8ToUtf16(val).c_str()) != 0;
#else
    ok = setenv(name.c_str(), val.c_str(), 1) == 0;
#endif
    if (!ok) g_lastError = "设置环境变量失败: " + name + " (" + sysErr() + ")";
    ae_push_bool(vm, ok ? 1 : 0);
    return 1;
}
static int nat_env(AeVM* vm, int, AeValue*) {
    AeValue t = ae_new_table(vm);
#ifdef _WIN32
    LPWSTR block = GetEnvironmentStringsW();
    if (!block) { g_lastError = "无法读取环境变量: " + sysErr(); ae_push_null(vm); return 1; }
    for (LPWSTR p = block; *p; p += wcslen(p) + 1) {
        std::wstring line = p;
        if (line[0] == L'=') continue;                 // 跳过隐藏变量
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        ae_table_set(vm, t, ae_str_value(vm, utf16ToUtf8(line.substr(0, eq))),
                            ae_str_value(vm, utf16ToUtf8(line.substr(eq + 1))));
    }
    FreeEnvironmentStringsW(block);
#else
    for (char** e = environ; e && *e; e++) {
        std::string line = *e;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        ae_table_set(vm, t, ae_str_value(vm, line.substr(0, eq)),
                            ae_str_value(vm, line.substr(eq + 1)));
    }
#endif
    ae_push_table(vm, t);
    return 1;
}

// ---------------------------------------------------------------------------
//  工作目录与常用目录
// ---------------------------------------------------------------------------
static int nat_cwd(AeVM* vm, int, AeValue*) {
#ifdef _WIN32
    DWORD need = GetCurrentDirectoryW(0, nullptr);
    std::wstring buf(need, L'\0');
    DWORD got = GetCurrentDirectoryW(need, &buf[0]);
    buf.resize(got);
    ae_push_str(vm, utf16ToUtf8(buf));
#else
    char buf[PATH_MAX];
    if (!getcwd(buf, sizeof(buf))) { g_lastError = "无法读取当前目录: " + sysErr(); ae_push_null(vm); return 1; }
    ae_push_str(vm, buf);
#endif
    return 1;
}
static int nat_chdir(AeVM* vm, int argc, AeValue* argv) {
    const std::string& p = argStr(vm, argv, argc, 0, "chdir");
    bool ok;
#ifdef _WIN32
    ok = SetCurrentDirectoryW(utf8ToUtf16(p).c_str()) != 0;
#else
    ok = chdir(p.c_str()) == 0;
#endif
    if (!ok) g_lastError = "切换目录失败: " + p + " (" + sysErr() + ")";
    ae_push_bool(vm, ok ? 1 : 0);
    return 1;
}
static int nat_tempDir(AeVM* vm, int, AeValue*) {
#ifdef _WIN32
    wchar_t buf[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    std::wstring w(buf, n);
    while (!w.empty() && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
    ae_push_str(vm, utf16ToUtf8(w));
#else
    const char* t = getenv("TMPDIR");
    std::string s = t ? t : "/tmp";
    while (!s.empty() && s.back() == '/') s.pop_back();
    ae_push_str(vm, s);
#endif
    return 1;
}
static int nat_homeDir(AeVM* vm, int, AeValue*) {
#ifdef _WIN32
    const char* v = getenv("USERPROFILE");
#else
    const char* v = getenv("HOME");
#endif
    if (!v) { g_lastError = "未设置用户主目录环境变量"; ae_push_null(vm); return 1; }
    ae_push_str(vm, v);
    return 1;
}

// ---------------------------------------------------------------------------
//  时间
// ---------------------------------------------------------------------------
static int nat_time(AeVM* vm, int, AeValue*) {
    ae_push_int(vm, (int64_t)::time(nullptr));
    return 1;
}
static int nat_millis(AeVM* vm, int, AeValue*) {
    using namespace std::chrono;
    static const auto origin = steady_clock::now();
    ae_push_int(vm, (int64_t)duration_cast<milliseconds>(steady_clock::now() - origin).count());
    return 1;
}
static int nat_sleep(AeVM* vm, int argc, AeValue* argv) {
    int64_t ms = argInt(vm, argv, argc, 0, "sleep");
    if (ms < 0) { ae_raise(vm, "sleep: 毫秒数必须 >= 0"); return 0; }
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)(ms * 1000));
#endif
    ae_push_null(vm);
    return 1;
}
static int nat_formatTime(AeVM* vm, int argc, AeValue* argv) {
    const std::string& fmt = argStr(vm, argv, argc, 0, "formatTime");
    time_t t = (argc >= 2) ? (time_t)argInt(vm, argv, argc, 1, "formatTime") : ::time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    if (localtime_s(&tmv, &t) != 0) { g_lastError = "时间转换失败"; ae_push_null(vm); return 1; }
#else
    if (!localtime_r(&t, &tmv)) { g_lastError = "时间转换失败"; ae_push_null(vm); return 1; }
#endif
    char buf[512];
    size_t n = strftime(buf, sizeof(buf), fmt.c_str(), &tmv);
    if (n == 0) { g_lastError = "格式化结果为空（格式串过长或无效）"; ae_push_str(vm, ""); return 1; }
    ae_push_str(vm, std::string(buf, n));
    return 1;
}

// ---------------------------------------------------------------------------
//  随机数
// ---------------------------------------------------------------------------
namespace {
std::mt19937_64& rng() {
    static std::mt19937_64 g([] { std::random_device rd; return ((uint64_t)rd() << 32) ^ rd(); }());
    return g;
}
}
static int nat_seed(AeVM* vm, int argc, AeValue* argv) {
    int64_t s = argInt(vm, argv, argc, 0, "seed");
    rng().seed((uint64_t)s);
    ae_push_null(vm);
    return 1;
}
static int nat_random(AeVM* vm, int argc, AeValue* argv) {
    if (argc == 0) {
        ae_push_int(vm, (int64_t)(rng()() & 0x7FFFFFFF));      // [0, 2^31-1]
        return 1;
    }
    int64_t n = argInt(vm, argv, argc, 0, "random");
    if (n <= 0) { ae_raise(vm, "random(n): n 必须 > 0，得到 " + std::to_string(n)); return 0; }
    std::uniform_int_distribution<int64_t> d(0, n - 1);
    ae_push_int(vm, d(rng()));
    return 1;
}
static int nat_randomFloat(AeVM* vm, int, AeValue*) {
    std::uniform_real_distribution<double> d(0.0, 1.0);
    ae_push_float(vm, d(rng()));
    return 1;
}

// ---------------------------------------------------------------------------
//  系统信息
// ---------------------------------------------------------------------------
static int nat_platform(AeVM* vm, int, AeValue*) {
#if defined(_WIN32)
    ae_push_str(vm, "windows");
#elif defined(__APPLE__)
    ae_push_str(vm, "macos");
#else
    ae_push_str(vm, "linux");
#endif
    return 1;
}
static int nat_pid(AeVM* vm, int, AeValue*) {
#ifdef _WIN32
    ae_push_int(vm, (int64_t)GetCurrentProcessId());
#else
    ae_push_int(vm, (int64_t)getpid());
#endif
    return 1;
}
static int nat_hostname(AeVM* vm, int, AeValue*) {
#ifdef _WIN32
    wchar_t buf[256];
    DWORD n = 256;
    if (GetComputerNameW(buf, &n)) ae_push_str(vm, utf16ToUtf8(std::wstring(buf, n)));
    else { g_lastError = "读取主机名失败: " + sysErr(); ae_push_null(vm); }
#else
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) { buf[sizeof(buf) - 1] = '\0'; ae_push_str(vm, buf); }
    else { g_lastError = "读取主机名失败: " + sysErr(); ae_push_null(vm); }
#endif
    return 1;
}
static int nat_cpuCount(AeVM* vm, int, AeValue*) {
    unsigned n = std::thread::hardware_concurrency();
    ae_push_int(vm, (int64_t)(n ? n : 1));
    return 1;
}
static int nat_lastError(AeVM* vm, int, AeValue*) {
    ae_push_str(vm, g_lastError);
    return 1;
}

// ---------------------------------------------------------------------------
//  导出表
// ---------------------------------------------------------------------------
static const AeNativeDef kExports[] = {
    // 命令行
    { "args",        0, 0, nat_args        },
    { "argCount",    0, 0, nat_argCount    },
    { "scriptPath",  0, 0, nat_scriptPath  },
    // 环境变量
    { "getEnv",      1, 1, nat_getEnv      },
    { "setEnv",      2, 2, nat_setEnv      },
    { "env",         0, 0, nat_env         },
    // 目录
    { "cwd",         0, 0, nat_cwd         },
    { "chdir",       1, 1, nat_chdir       },
    { "tempDir",     0, 0, nat_tempDir     },
    { "homeDir",     0, 0, nat_homeDir     },
    // 时间
    { "time",        0, 0, nat_time        },
    { "millis",      0, 0, nat_millis      },
    { "sleep",       1, 1, nat_sleep       },
    { "formatTime",  1, 2, nat_formatTime  },
    // 随机
    { "seed",        1, 1, nat_seed        },
    { "random",      0, 1, nat_random      },
    { "randomFloat", 0, 0, nat_randomFloat },
    // 系统信息
    { "platform",    0, 0, nat_platform    },
    { "pid",         0, 0, nat_pid         },
    { "hostname",    0, 0, nat_hostname    },
    { "cpuCount",    0, 0, nat_cpuCount    },
    { "lastError",   0, 0, nat_lastError   },
};

static const AeNativeDef* os_getExports(int* count) {
    *count = (int)(sizeof(kExports) / sizeof(kExports[0]));
    return kExports;
}

AE_MODULE("os", os_getExports)
