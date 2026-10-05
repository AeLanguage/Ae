// =============================================================================
//  ae_console.h —— 控制台中文（编码）修复
// -----------------------------------------------------------------------------
//  问题
//    Windows 控制台默认代码页是 936(GBK) / 437 等，而工具链内部【一律用 UTF-8】。
//    cout << "中文" 写出去的是 UTF-8 字节，cmd 按 GBK 解释 → ASCII 正常、中文乱码。
//    脚本的 pr/ prln 和工具链自己的诊断信息都走 cout/cerr，所以两边都乱。
//
//  做法（Windows）
//    · stdout/stderr 是【控制台】→ 把 cout/cerr 的底层缓冲换成
//      "UTF-8 → UTF-16 → WriteConsoleW"：WriteConsoleW 直接跟控制台会话打交道，
//      【不经过代码页】，因此 GBK 的 cmd 里中文也正确。
//      同时刻意【不】改控制台自己的代码页 —— 那会影响同一个窗口里跑的其他程序。
//    · stdout/stderr 是【管道/文件】（重定向）→ 完全不动，照旧写 UTF-8 字节。
//      所以 `ae x.aeo > out.txt`、测试脚本读文件的行为都没变。
//    · stdin 是【控制台】→ 换成 ReadConsoleW 读一行再转 UTF-8，
//      这样在 GBK 的 cmd 里手打中文给 inp() 也不会乱（同样不改输入代码页）。
//
//  用法：在 main() 的第一行调用 aecon::install();
//    · 非 Windows 平台是空实现
//    · 换下来的原缓冲会被静态持有（不能让它析构）
//    · WriteConsoleW 失败时自动退回原缓冲写原始字节，不会丢输出
// =============================================================================
#ifndef AE_CONSOLE_H
#define AE_CONSOLE_H

#include <iostream>
#include <streambuf>
#include <string>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>

namespace aecon {
namespace detail {

inline bool isConsole(DWORD which) {
    HANDLE h = GetStdHandle(which);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return false;
    DWORD mode = 0;
    return GetConsoleMode(h, &mode) != 0;      // 只有真控制台才有 mode
}

inline std::wstring toWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

inline std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// ── 输出：UTF-8 字节累积 → flush 时转 UTF-16 交给控制台 ──────────────────────
class OutBuf : public std::streambuf {
public:
    OutBuf(HANDLE h, std::streambuf* fallback) : h_(h), fb_(fallback) {}

protected:
    int_type overflow(int_type c) override {
        if (!traits_type::eq_int_type(c, traits_type::eof())) buf_.push_back((char)c);
        return traits_type::not_eof(c);
    }

    int sync() override { return flushAll() ? 0 : -1; }

private:
    bool flushAll() {
        if (buf_.empty()) return true;
        std::string bytes;
        bytes.swap(buf_);                       // 先取出，失败时还能兜底写出
        std::wstring w = toWide(bytes);
        if (w.empty()) { writeRaw(bytes); return true; }

        size_t off = 0;
        while (off < w.size()) {
            DWORD chunk = (DWORD)((w.size() - off) > 8000 ? 8000 : (w.size() - off));
            DWORD wrote = 0;
            if (!WriteConsoleW(h_, w.data() + off, chunk, &wrote, nullptr) || wrote == 0) {
                // 控制台写不进去 → 把剩下的按原始 UTF-8 字节退回原缓冲，至少不丢输出
                writeRaw(toUtf8(w.substr(off)));
                return true;
            }
            off += wrote;
        }
        return true;
    }

    void writeRaw(const std::string& bytes) {
        if (fb_ && !bytes.empty()) fb_->sputn(bytes.data(), (std::streamsize)bytes.size());
    }

    HANDLE        h_;
    std::streambuf* fb_;                        // 原缓冲：只用于兜底
    std::string   buf_;
};

// ── 输入：ReadConsoleW 读一行（控制台负责回显/退格）→ 转 UTF-8 ───────────────
class InBuf : public std::streambuf {
public:
    explicit InBuf(HANDLE h) : h_(h) {}

protected:
    int_type underflow() override {
        if (pos_ < line_.size()) return (unsigned char)line_[pos_];
        if (!readLine()) return traits_type::eof();
        return (unsigned char)line_[pos_];
    }

    int_type uflow() override {
        int_type c = underflow();
        if (!traits_type::eq_int_type(c, traits_type::eof())) pos_++;
        return c;
    }

private:
    bool readLine() {
        line_.clear();
        pos_ = 0;
        std::wstring w;
        for (;;) {
            wchar_t buf[1024];
            DWORD got = 0;
            if (!ReadConsoleW(h_, buf, 1023, &got, nullptr) || got == 0)
                return false;                   // Ctrl+Z / 出错 → EOF
            w.append(buf, got);
            if (got < 1023) break;              // 一行读完（默认含 \r\n）
        }
        line_ = toUtf8(w);
        // 规范化行尾：CRLF / CR / LF 一律只留 LF，getline 拿到的行才不带 \r
        while (!line_.empty() && (line_.back() == '\n' || line_.back() == '\r'))
            line_.pop_back();
        line_.push_back('\n');
        return true;
    }

    HANDLE      h_;
    std::string line_;
    size_t      pos_ = 0;
};

}  // namespace detail

inline void install() {
    if (detail::isConsole(STD_OUTPUT_HANDLE)) {
        static detail::OutBuf ob(GetStdHandle(STD_OUTPUT_HANDLE), std::cout.rdbuf());
        std::cout.rdbuf(&ob);
        // 每次 << 立刻输出：pr("提示: ") 这种不带换行的提示必须马上可见
        std::cout << std::unitbuf;
    }
    if (detail::isConsole(STD_ERROR_HANDLE)) {
        static detail::OutBuf eb(GetStdHandle(STD_ERROR_HANDLE), std::cerr.rdbuf());
        std::cerr.rdbuf(&eb);
        std::cerr << std::unitbuf;
    }
    if (detail::isConsole(STD_INPUT_HANDLE)) {
        static detail::InBuf ib(GetStdHandle(STD_INPUT_HANDLE));
        std::cin.rdbuf(&ib);
    }
}

}  // namespace aecon

#else   // 非 Windows：什么都不用做（终端本身就是 UTF-8）

namespace aecon { inline void install() {} }

#endif  // _WIN32
#endif  // AE_CONSOLE_H
