// =============================================================================
//  io —— Ae 的第一个原生库（文件读写）
// -----------------------------------------------------------------------------
//  设计取舍
//    · 无异常/无闭包，所以采用「返回 null/false + lastError()」报告【预期失败】
//      （文件不存在、目录读不了…），而参数类型错、句柄非法这类【程序性错误】
//      直接 ae_raise，变成带调用栈的运行错误。
//    · 文本按 UTF-8 字节原样处理，**不做换行转换**（Windows 上也用二进制模式），
//      这样 \n 在磁盘上就是 \n，跨平台行为一致；readLine 会吃掉结尾的 \r\n 或 \n。
//    · 句柄是 int（从 1 开始递增），由本库内部表管理；脚本传进非法句柄会报错。
//      句柄不会自动回收，用完请 close（进程退出时一并释放）。
//    · Windows 下路径按 UTF-8 传入，内部转 UTF-16，所以中文路径可用。
//
//  编译（在 Apt/lib 目录下）：
//      clang++ -std=c++17 -O2 -shared io.cpp -o io.dll
//
//  脚本侧用法：
//      imp io
//      local text = readFile("a.txt")          // 导入后可直接裸调
//      if (text == null) { prln("读取失败:", io.lastError()) }
//      else { prln(len(text)) }
//      io.writeFile("b.txt", "hello")          // 也可以带库名精确调用
// =============================================================================
#ifdef _WIN32
  #define _CRT_SECURE_NO_WARNINGS
#endif

#ifdef _WIN32
  #define _CRT_SECURE_NO_WARNINGS
#endif
#include "../ae_native.h"
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <memory>

#ifdef _WIN32
  #include <windows.h>
  #include <io.h>
  #define AE_FSEEK64 _fseeki64
  #define AE_FTELL64 _ftelli64
#else
  #include <sys/stat.h>
  #include <dirent.h>
  #include <unistd.h>
  #define AE_FSEEK64 fseeko
  #define AE_FTELL64 ftello
#endif

// ---------------------------------------------------------------------------
//  工具
// ---------------------------------------------------------------------------
static std::string g_lastError;

static void fail(AeVM* /*vm*/, const std::string& why) {
    g_lastError = why;
}

// UTF-8 → 宽字符（Windows 用，保证中文路径可用）
#ifdef _WIN32
  typedef const wchar_t* AeMode;
  #define AE_M_RB L"rb"
  #define AE_M_WB L"wb"
  #define AE_M_AB L"ab"
  static std::wstring toWide(const std::string& s) {
      if (s.empty()) return std::wstring();
      int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
      std::wstring w((size_t)n, L'\0');
      MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
      return w;
  }
  static FILE* openFile(const std::string& path, AeMode mode) {
      return _wfopen(toWide(path).c_str(), mode);
  }
  static std::string sysErr() {
      DWORD e = GetLastError();
      if (!e) return "系统调用失败";
      // ★ 修：用 FormatMessageW 拿宽字符再转 UTF-8。以前用 FormatMessageA，
      //   返回的是本机 ANSI 代码页（中文 Windows 上是 GBK）的文本，却被当成
      //   UTF-8 字符串存进 lastError —— 打印出来是「ϵͳ�Ҳ���ָ�����ļ���」这种乱码。
      LPWSTR wbuf = nullptr;
      DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                               FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, (LPWSTR)&wbuf, 0, nullptr);
      std::string msg;
      if (n && wbuf) {
          int bytes = WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)n, nullptr, 0, nullptr, nullptr);
          if (bytes > 0) {
              msg.resize((size_t)bytes);
              WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)n, &msg[0], bytes, nullptr, nullptr);
          }
          LocalFree(wbuf);
      }
      if (msg.empty()) msg = "错误码 " + std::to_string(e);
      while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' ')) msg.pop_back();
      return msg;
  }
#else
  typedef const char* AeMode;
  #define AE_M_RB "rb"
  #define AE_M_WB "wb"
  #define AE_M_AB "ab"
  static FILE* openFile(const std::string& path, AeMode mode) { return fopen(path.c_str(), mode); }
  static std::string sysErr() { return std::strerror(errno); }
#endif

static bool fileExists(const std::string& path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesW(toWide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0;
#endif
}
static bool isDirectory(const std::string& path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesW(toWide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

// ---------------------------------------------------------------------------
//  参数检查助手
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
//  句柄表
// ---------------------------------------------------------------------------
namespace {
struct FileHandle {
    FILE* fp = nullptr;
    std::string path;
};
std::vector<std::unique_ptr<FileHandle>> g_files;   // 下标+1 = 句柄

int allocHandle(FILE* fp, const std::string& path) {
    for (size_t i = 0; i < g_files.size(); i++) {
        if (!g_files[i]) { g_files[i].reset(new FileHandle{fp, path}); return (int)i + 1; }
    }
    g_files.push_back(std::unique_ptr<FileHandle>(new FileHandle{fp, path}));
    return (int)g_files.size();
}
FileHandle* getHandle(AeVM* vm, int64_t h, const char* fn) {
    if (h < 1 || (size_t)h > g_files.size() || !g_files[(size_t)h - 1]) {
        ae_raise(vm, std::string(fn) + ": 无效的文件句柄 " + std::to_string(h) +
                     "（句柄由 open() 返回，用完要 close()）");
        return nullptr;
    }
    return g_files[(size_t)h - 1].get();
}
}  // namespace

// ---------------------------------------------------------------------------
//  整文件 API
// ---------------------------------------------------------------------------
static int nat_readFile(AeVM* vm, int argc, AeValue* argv) {
    const std::string& path = argStr(vm, argv, argc, 0, "readFile");
    FILE* fp = openFile(path, AE_M_RB);
    if (!fp) { fail(vm, "无法打开 " + path + ": " + sysErr()); ae_push_null(vm); return 1; }
    std::string out;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
    bool bad = ferror(fp) != 0;
    fclose(fp);
    if (bad) { fail(vm, "读取 " + path + " 时出错: " + sysErr()); ae_push_null(vm); return 1; }
    ae_push_str(vm, out);
    return 1;
}

static int nat_writeFile(AeVM* vm, int argc, AeValue* argv) {
    const std::string& path = argStr(vm, argv, argc, 0, "writeFile");
    const std::string& text = argStr(vm, argv, argc, 1, "writeFile");
    FILE* fp = openFile(path, AE_M_WB);
    if (!fp) { fail(vm, "无法写入 " + path + ": " + sysErr()); ae_push_bool(vm, 0); return 1; }
    bool ok = text.empty() || fwrite(text.data(), 1, text.size(), fp) == text.size();
    if (fclose(fp) != 0) ok = false;
    if (!ok) fail(vm, "写入 " + path + " 失败: " + sysErr());
    ae_push_bool(vm, ok ? 1 : 0);
    return 1;
}

static int nat_appendFile(AeVM* vm, int argc, AeValue* argv) {
    const std::string& path = argStr(vm, argv, argc, 0, "appendFile");
    const std::string& text = argStr(vm, argv, argc, 1, "appendFile");
    FILE* fp = openFile(path, AE_M_AB);
    if (!fp) { fail(vm, "无法追加写入 " + path + ": " + sysErr()); ae_push_bool(vm, 0); return 1; }
    bool ok = text.empty() || fwrite(text.data(), 1, text.size(), fp) == text.size();
    if (fclose(fp) != 0) ok = false;
    if (!ok) fail(vm, "追加写入 " + path + " 失败: " + sysErr());
    ae_push_bool(vm, ok ? 1 : 0);
    return 1;
}

// 按行读入整个文件（丢掉行尾换行；不保留行尾的 \r）
static int nat_readLines(AeVM* vm, int argc, AeValue* argv) {
    const std::string& path = argStr(vm, argv, argc, 0, "readLines");
    FILE* fp = openFile(path, AE_M_RB);
    if (!fp) { fail(vm, "无法打开 " + path + ": " + sysErr()); ae_push_null(vm); return 1; }
    AeValue t = ae_new_table(vm);
    std::string line;
    int64_t idx = 0;
    int c;
    bool any = false;
    while ((c = fgetc(fp)) != EOF) {
        any = true;
        if (c == '\n') {
            ae_table_set(vm, t, ae_int_value(++idx), ae_str_value(vm, line));
            line.clear();
        } else if (c != '\r') {
            line += (char)c;
        }
    }
    if (!line.empty() || (any && idx == 0)) ae_table_set(vm, t, ae_int_value(++idx), ae_str_value(vm, line));
    fclose(fp);
    ae_push_table(vm, t);
    return 1;
}

// ---------------------------------------------------------------------------
//  元信息与目录
// ---------------------------------------------------------------------------
static int nat_exists(AeVM* vm, int argc, AeValue* argv) {
    ae_push_bool(vm, fileExists(argStr(vm, argv, argc, 0, "exists")) ? 1 : 0);
    return 1;
}
static int nat_isFile(AeVM* vm, int argc, AeValue* argv) {
    const std::string& p = argStr(vm, argv, argc, 0, "isFile");
    ae_push_bool(vm, (fileExists(p) && !isDirectory(p)) ? 1 : 0);
    return 1;
}
static int nat_isDir(AeVM* vm, int argc, AeValue* argv) {
    ae_push_bool(vm, isDirectory(argStr(vm, argv, argc, 0, "isDir")) ? 1 : 0);
    return 1;
}
static int nat_size(AeVM* vm, int argc, AeValue* argv) {
    const std::string& p = argStr(vm, argv, argc, 0, "size");
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(toWide(p).c_str(), GetFileExInfoStandard, &d)) {
        fail(vm, "stat " + p + " 失败: " + sysErr()); ae_push_null(vm); return 1;
    }
    int64_t sz = ((int64_t)d.nFileSizeHigh << 32) | (int64_t)d.nFileSizeLow;
#else
    struct stat st;
    if (stat(p.c_str(), &st) != 0) { fail(vm, "stat " + p + " 失败: " + sysErr()); ae_push_null(vm); return 1; }
    int64_t sz = (int64_t)st.st_size;
#endif
    ae_push_int(vm, sz);
    return 1;
}
static int nat_listDir(AeVM* vm, int argc, AeValue* argv) {
    const std::string& path = argStr(vm, argv, argc, 0, "listDir");
    AeValue t = ae_new_table(vm);
    int64_t idx = 0;
#ifdef _WIN32
    std::wstring pattern = toWide(path) + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        fail(vm, "无法列出目录 " + path + ": " + sysErr()); ae_push_null(vm); return 1;
    }
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        int need = WideCharToMultiByte(CP_UTF8, 0, n.c_str(), (int)n.size(), nullptr, 0, nullptr, nullptr);
        std::string u((size_t)need, '\0');
        WideCharToMultiByte(CP_UTF8, 0, n.c_str(), (int)n.size(), &u[0], need, nullptr, nullptr);
        ae_table_set(vm, t, ae_int_value(++idx), ae_str_value(vm, u));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(path.c_str());
    if (!d) { fail(vm, "无法列出目录 " + path + ": " + sysErr()); ae_push_null(vm); return 1; }
    struct dirent* e;
    while ((e = readdir(d))) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        ae_table_set(vm, t, ae_int_value(++idx), ae_str_value(vm, n));
    }
    closedir(d);
#endif
    ae_push_table(vm, t);
    return 1;
}
static int nat_mkdir(AeVM* vm, int argc, AeValue* argv) {
    const std::string& p = argStr(vm, argv, argc, 0, "mkdir");
    // 逐级创建（等价 mkdir -p）
    std::string cur;
    bool ok = true;
    for (size_t i = 0; i < p.size(); i++) {
        cur += p[i];
        if (p[i] == '/' || p[i] == '\\' || i + 1 == p.size()) {
            std::string d = cur;
            while (!d.empty() && (d.back() == '/' || d.back() == '\\')) d.pop_back();
            if (d.empty() || d.size() == 2 && d[1] == ':') continue;
            if (isDirectory(d)) continue;
#ifdef _WIN32
            if (!CreateDirectoryW(toWide(d).c_str(), nullptr)) { ok = false; }
#else
            if (mkdir(d.c_str(), 0755) != 0) { ok = false; }
#endif
        }
    }
    if (!ok) fail(vm, "创建目录 " + p + " 失败: " + sysErr());
    ae_push_bool(vm, ok ? 1 : 0);
    return 1;
}
static int nat_remove(AeVM* vm, int argc, AeValue* argv) {
    const std::string& p = argStr(vm, argv, argc, 0, "remove");
    bool ok = false;
#ifdef _WIN32
    if (isDirectory(p)) ok = RemoveDirectoryW(toWide(p).c_str()) != 0;
    else                ok = DeleteFileW(toWide(p).c_str()) != 0;
#else
    if (isDirectory(p)) ok = rmdir(p.c_str()) == 0;
    else                ok = unlink(p.c_str()) == 0;
#endif
    if (!ok) fail(vm, "删除 " + p + " 失败: " + sysErr());
    ae_push_bool(vm, ok ? 1 : 0);
    return 1;
}
static int nat_copyFile(AeVM* vm, int argc, AeValue* argv) {
    const std::string& a = argStr(vm, argv, argc, 0, "copyFile");
    const std::string& b = argStr(vm, argv, argc, 1, "copyFile");
    bool ok = false;
#ifdef _WIN32
    ok = CopyFileW(toWide(a).c_str(), toWide(b).c_str(), FALSE) != 0;
#else
    FILE* in = fopen(a.c_str(), "rb"); FILE* out = in ? fopen(b.c_str(), "wb") : nullptr;
    if (in && out) { char buf[65536]; size_t n; ok = true;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) if (fwrite(buf, 1, n, out) != n) { ok = false; break; } }
    if (in) fclose(in); if (out) fclose(out);
#endif
    if (!ok) fail(vm, "复制 " + a + " → " + b + " 失败: " + sysErr());
    ae_push_bool(vm, ok ? 1 : 0);
    return 1;
}
static int nat_rename(AeVM* vm, int argc, AeValue* argv) {
    const std::string& a = argStr(vm, argv, argc, 0, "rename");
    const std::string& b = argStr(vm, argv, argc, 1, "rename");
#ifdef _WIN32
    bool ok = MoveFileExW(toWide(a).c_str(), toWide(b).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    bool ok = std::rename(a.c_str(), b.c_str()) == 0;
#endif
    if (!ok) fail(vm, "重命名 " + a + " → " + b + " 失败: " + sysErr());
    ae_push_bool(vm, ok ? 1 : 0);
    return 1;
}

// ---------------------------------------------------------------------------
//  流式 API
// ---------------------------------------------------------------------------
static int nat_open(AeVM* vm, int argc, AeValue* argv) {
    const std::string& path = argStr(vm, argv, argc, 0, "open");
    const std::string& mode = argStr(vm, argv, argc, 1, "open");
    AeMode wm = AE_M_RB;
    if (mode == "r")      wm = AE_M_RB;
    else if (mode == "w") wm = AE_M_WB;
    else if (mode == "a") wm = AE_M_AB;
    else { ae_raise(vm, "open: 模式只能是 \"r\" / \"w\" / \"a\"，得到 \"" + mode + "\""); return 0; }
    FILE* fp = openFile(path, wm);
    if (!fp) { fail(vm, "无法按模式 " + mode + " 打开 " + path + ": " + sysErr()); ae_push_null(vm); return 1; }
    ae_push_int(vm, allocHandle(fp, path));
    return 1;
}
static int nat_close(AeVM* vm, int argc, AeValue* argv) {
    int64_t h = argInt(vm, argv, argc, 0, "close");
    FileHandle* fh = getHandle(vm, h, "close");
    if (!fh) return 0;
    bool ok = fclose(fh->fp) == 0;
    g_files[(size_t)h - 1].reset();
    if (!ok) fail(vm, "关闭句柄 " + std::to_string(h) + " 失败: " + sysErr());
    ae_push_bool(vm, ok ? 1 : 0);
    return 1;
}
static int nat_readLine(AeVM* vm, int argc, AeValue* argv) {
    int64_t h = argInt(vm, argv, argc, 0, "readLine");
    FileHandle* fh = getHandle(vm, h, "readLine");
    if (!fh) return 0;
    std::string line;
    int c = fgetc(fh->fp);
    if (c == EOF && line.empty()) { ae_push_null(vm); return 1; }   // 已到文件末尾
    while (c != EOF && c != '\n') {
        if (c != '\r') line += (char)c;
        c = fgetc(fh->fp);
    }
    ae_push_str(vm, line);
    return 1;
}
static int nat_readAll(AeVM* vm, int argc, AeValue* argv) {
    int64_t h = argInt(vm, argv, argc, 0, "readAll");
    FileHandle* fh = getHandle(vm, h, "readAll");
    if (!fh) return 0;
    std::string out;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fh->fp)) > 0) out.append(buf, n);
    ae_push_str(vm, out);
    return 1;
}
static int nat_readBytes(AeVM* vm, int argc, AeValue* argv) {
    int64_t h = argInt(vm, argv, argc, 0, "readBytes");
    int64_t want = argInt(vm, argv, argc, 1, "readBytes");
    FileHandle* fh = getHandle(vm, h, "readBytes");
    if (!fh) return 0;
    if (want < 0) { ae_raise(vm, "readBytes: 字节数必须 >= 0"); return 0; }
    std::string out((size_t)want, '\0');
    size_t got = want ? fread(&out[0], 1, (size_t)want, fh->fp) : 0;
    out.resize(got);
    ae_push_str(vm, out);
    return 1;
}
static int nat_write(AeVM* vm, int argc, AeValue* argv) {
    int64_t h = argInt(vm, argv, argc, 0, "write");
    const std::string& text = argStr(vm, argv, argc, 1, "write");
    FileHandle* fh = getHandle(vm, h, "write");
    if (!fh) return 0;
    size_t n = text.empty() ? 0 : fwrite(text.data(), 1, text.size(), fh->fp);
    if (n != text.size()) { fail(vm, "写入句柄 " + std::to_string(h) + " 失败: " + sysErr()); ae_push_null(vm); return 1; }
    ae_push_int(vm, (int64_t)n);
    return 1;
}
static int nat_seek(AeVM* vm, int argc, AeValue* argv) {
    int64_t h = argInt(vm, argv, argc, 0, "seek");
    int64_t pos = argInt(vm, argv, argc, 1, "seek");
    FileHandle* fh = getHandle(vm, h, "seek");
    if (!fh) return 0;
    bool ok = AE_FSEEK64(fh->fp, pos, SEEK_SET) == 0;
    if (!ok) fail(vm, "seek 失败: " + sysErr());
    ae_push_bool(vm, ok ? 1 : 0);
    return 1;
}
static int nat_tell(AeVM* vm, int argc, AeValue* argv) {
    int64_t h = argInt(vm, argv, argc, 0, "tell");
    FileHandle* fh = getHandle(vm, h, "tell");
    if (!fh) return 0;
    ae_push_int(vm, (int64_t)AE_FTELL64(fh->fp));
    return 1;
}
static int nat_flush(AeVM* vm, int argc, AeValue* argv) {
    int64_t h = argInt(vm, argv, argc, 0, "flush");
    FileHandle* fh = getHandle(vm, h, "flush");
    if (!fh) return 0;
    bool ok = fflush(fh->fp) == 0;
    if (!ok) fail(vm, "flush 失败: " + sysErr());
    ae_push_bool(vm, ok ? 1 : 0);
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
    // 整文件
    { "readFile",   1, 1, nat_readFile   },
    { "writeFile",  2, 2, nat_writeFile  },
    { "appendFile", 2, 2, nat_appendFile },
    { "readLines",  1, 1, nat_readLines  },
    // 元信息与目录
    { "exists",     1, 1, nat_exists     },
    { "isFile",     1, 1, nat_isFile     },
    { "isDir",      1, 1, nat_isDir      },
    { "size",       1, 1, nat_size       },
    { "listDir",    1, 1, nat_listDir    },
    { "mkdir",      1, 1, nat_mkdir      },
    { "remove",     1, 1, nat_remove     },
    { "copyFile",   2, 2, nat_copyFile   },
    { "rename",     2, 2, nat_rename     },
    // 流式
    { "open",       2, 2, nat_open       },
    { "close",      1, 1, nat_close      },
    { "readLine",   1, 1, nat_readLine   },
    { "readAll",    1, 1, nat_readAll    },
    { "readBytes",  2, 2, nat_readBytes  },
    { "write",      2, 2, nat_write      },
    { "seek",       2, 2, nat_seek       },
    { "tell",       1, 1, nat_tell       },
    { "flush",      1, 1, nat_flush      },
    // 错误查询
    { "lastError",  0, 0, nat_lastError  },
};

static const AeNativeDef* io_getExports(int* count) {
    *count = (int)(sizeof(kExports) / sizeof(kExports[0]));
    return kExports;
}

AE_MODULE("io", io_getExports)
