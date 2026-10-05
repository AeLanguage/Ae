// =============================================================================
//  conprobe —— 验证"GBK(936) 控制台里 Ae 的中文"
// -----------------------------------------------------------------------------
//  这个检查存在的理由：控制台编码问题是【看不见的】—— 在管道/重定向里永远是对的，
//  只有在真控制台 + 非 UTF-8 代码页时才出问题。所以这里自己开一个隐藏控制台、
//  把代码页设成 936（中文 cmd 的默认值）跑子进程，再用
//  ReadConsoleOutputCharacterW 读【屏幕缓冲区里真正的字符】来判定，
//  而不是"看日志像不像"。
//
//  逐项验证：
//    1. 脚本输出（pr / prln）
//    2. 工具链自己的输出（ae --help，走 cout）
//    3. 工具链的诊断信息（aec 编译错误，走 cerr + 源码摘录 + 波浪线）
//    4. 手输中文（WriteConsoleInputW 模拟键盘 → inp() 读回来再打印）
//    5. 对照组：老做法（WriteFile 直写 UTF-8 字节）在 936 下确实是乱码
//
//  用法（一般由 Test/check_console.ps1 调用）：
//    conprobe.exe <报告文件> <Apt 目录> <工作目录>
//  退出码 0 = 全通过，2 = 有失败
// =============================================================================
#include <windows.h>
#include <string>
#include <vector>
#include <fstream>

static std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// 读屏幕缓冲区第 y0 行到光标行（行尾空格去掉）
static std::wstring readScreenFrom(HANDLE hOut, SHORT y0) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(hOut, &info)) return L"<读屏幕失败>";
    SHORT cols = info.dwSize.X;
    SHORT rows = (SHORT)(info.dwCursorPosition.Y + 1);
    std::wstring all;
    for (SHORT y = y0; y < rows; y++) {
        std::vector<wchar_t> buf((size_t)cols + 1, 0);
        DWORD got = 0;
        COORD pos; pos.X = 0; pos.Y = y;
        if (!ReadConsoleOutputCharacterW(hOut, buf.data(), (DWORD)cols, pos, &got)) break;
        std::wstring line(buf.data(), got);
        while (!line.empty() && (line.back() == L' ' || line.back() == L'\0')) line.pop_back();
        all += line;
        all += L"\n";
    }
    return all;
}

static SHORT cursorRow(HANDLE hOut) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(hOut, &info)) return 0;
    return info.dwCursorPosition.Y;
}

static bool has(const std::wstring& hay, const std::wstring& needle) {
    return hay.find(needle) != std::wstring::npos;
}

struct Result { std::wstring name; std::wstring detail; bool ok; };
static std::vector<Result> g_results;

static void check(const std::wstring& name, const std::wstring& screen,
                  const std::wstring& expect, bool expectHit = true) {
    bool hit = has(screen, expect);
    Result r;
    r.name   = name;
    r.ok     = (hit == expectHit);
    r.detail = (hit ? L"命中" : L"没命中") + std::wstring(L"「") + expect + L"」";
    g_results.push_back(r);
}

static DWORD runChild(const std::wstring& cmdline, const std::wstring& cwd) {
    std::wstring cmd = cmdline;
    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, 0,
                        nullptr, cwd.c_str(), &si, &pi))
        return (DWORD)-1;
    WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

// 往控制台输入缓冲区塞按键（模拟手打），最后回车
static void typeText(HANDLE hIn, const std::wstring& text) {
    std::vector<INPUT_RECORD> recs;
    for (wchar_t ch : text) {
        INPUT_RECORD r; ZeroMemory(&r, sizeof(r));
        r.EventType = KEY_EVENT;
        r.Event.KeyEvent.bKeyDown = TRUE;
        r.Event.KeyEvent.wRepeatCount = 1;
        r.Event.KeyEvent.uChar.UnicodeChar = ch;
        recs.push_back(r);
    }
    INPUT_RECORD enter; ZeroMemory(&enter, sizeof(enter));
    enter.EventType = KEY_EVENT;
    enter.Event.KeyEvent.bKeyDown = TRUE;
    enter.Event.KeyEvent.wRepeatCount = 1;
    enter.Event.KeyEvent.uChar.UnicodeChar = L'\r';
    enter.Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
    recs.push_back(enter);
    DWORD wrote = 0;
    WriteConsoleInputW(hIn, recs.data(), (DWORD)recs.size(), &wrote);
}

int main(int argc, char** argv) {
    std::string reportPath = (argc > 1) ? argv[1] : "conprobe_report.txt";
    std::wstring apt     = (argc > 2) ? fromUtf8(argv[2]) : L"..\\Apt";
    std::wstring work    = (argc > 3) ? fromUtf8(argv[3]) : L".";

    FreeConsole();
    if (!AllocConsole()) {
        std::ofstream f(reportPath.c_str(), std::ios::binary);
        f << "AllocConsole 失败（没有控制台可用）\n";
        return 2;
    }
    HWND cw = GetConsoleWindow();
    if (cw) ShowWindow(cw, SW_HIDE);            // 别弹窗打扰人
    SetConsoleOutputCP(936);                    // ★ 模拟中文 cmd 默认代码页
    SetConsoleCP(936);

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hIn  = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring report = L"控制台代码页 = " + std::to_wstring(GetConsoleOutputCP()) + L"\n\n";

    std::wstring ae  = apt + L"\\ae.exe";
    std::wstring aec = apt + L"\\aec.exe";

    // ── 1) 脚本输出 ──
    SHORT m = cursorRow(hOut);
    DWORD rc1 = runChild(ae + L" \"" + work + L"\\con_test.aeo\"", work);
    std::wstring s1 = readScreenFrom(hOut, m);
    check(L"脚本 prln 中文", s1, L"中文输出测试：你好，世界");
    check(L"脚本 ASCII",    s1, L"ASCII ok");
    check(L"脚本 pr 不换行拼接", s1, L"不带换行的中文提示: 该换行了");

    // ── 2) 工具链自己的输出（cout）──
    m = cursorRow(hOut);
    DWORD rc2 = runChild(ae + L" --help", work);
    std::wstring s2 = readScreenFrom(hOut, m);
    check(L"ae --help 中文",     s2, L"Ae 虚拟机 (ae) 1.1");
    check(L"ae --help 说明中文", s2, L"调用深度上限");
    check(L"ae --help 参数说明", s2, L"之后的参数原样传给脚本");

    // ── 3) 诊断信息（cerr + 源码摘录 + 波浪线）──
    m = cursorRow(hOut);
    DWORD rc3 = runChild(aec + L" \"" + work + L"\\con_bad.ae\"", work);
    std::wstring s3 = readScreenFrom(hOut, m);
    check(L"编译错误码",       s3, L"error[E1001]");
    check(L"编译错误中文",     s3, L"无法解析 '}'");
    check(L"编译错误波浪线",   s3, L"^");
    check(L"编译错误无替换字符", s3, L"\uFFFD", false);

    // ── 4) 手输中文（inp）──
    m = cursorRow(hOut);
    typeText(hIn, L"中文输入测试");
    DWORD rc4 = runChild(ae + L" \"" + work + L"\\con_in.aeo\"", work);
    std::wstring s4 = readScreenFrom(hOut, m);
    check(L"手输中文被正确读回", s4, L"收到 = 中文输入测试");
    check(L"输入提示文字",       s4, L"请输入: ");

    // ── 5) 对照：老做法直写 UTF-8 字节（应当乱码）──
    m = cursorRow(hOut);
    std::string oldWay = "\xe5\xaf\xb9\xe7\x85\xa7\xef\xbc\x9a\xe4\xb8\xad\xe6\x96\x87\xe6\xb5\x8b\xe8\xaf\x95\n";
    DWORD wrote = 0;
    WriteFile(hOut, oldWay.data(), (DWORD)oldWay.size(), &wrote, nullptr);
    std::wstring s5 = readScreenFrom(hOut, m);
    check(L"老做法直写 UTF-8（应当乱码）", s5, L"对照：中文测试", false);

    // ── 汇总 ──
    int pass = 0, fail = 0;
    for (auto& r : g_results) { if (r.ok) pass++; else fail++; }
    report += L"退出码: ae con_test=" + std::to_wstring(rc1) + L"  ae --help=" + std::to_wstring(rc2) +
              L"  aec con_bad=" + std::to_wstring(rc3) + L"  ae con_in=" + std::to_wstring(rc4) + L"\n\n";
    for (auto& r : g_results)
        report += std::wstring(r.ok ? L"[+] " : L"[-] ") + r.name + L" —— " + r.detail + L"\n";
    report += L"\n通过 " + std::to_wstring(pass) + L"，失败 " + std::to_wstring(fail) + L"\n";

    report += L"\n==== 第 1 段：脚本输出 ====\n" + s1;
    report += L"\n==== 第 2 段：ae --help（工具链 stdout）====\n" + s2;
    report += L"\n==== 第 3 段：编译错误诊断（工具链 stderr）====\n" + s3;
    report += L"\n==== 第 4 段：手输中文（inp）====\n" + s4;
    report += L"\n==== 第 5 段：老做法对照（应当乱码）====\n" + s5;

    std::string out = toUtf8(report);
    std::ofstream f(reportPath.c_str(), std::ios::binary);
    f.write(out.data(), (std::streamsize)out.size());
    f.close();
    return fail == 0 ? 0 : 2;
}
