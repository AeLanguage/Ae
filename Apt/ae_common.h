#pragma once
// Ae 编译器公共基础设施（诊断消息打包 / UTF-8 / 字节码写入）
// 从 aec.cpp 抽出，供 aec 与将来的 AOT 后端共用。
#include <string>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <cstring>
using namespace std;

// 结构化诊断消息：\x01 行 \x01 列 \x01 长度 \x01 消息
// 用它而不是改 20 多处 throw —— 错误分类集中在 renderDiag() 一处维护
static std::string aePack(int file, int line, int col, int len, const std::string& msg) {
    return std::string("\x01") + std::to_string(file) + "\x01" + std::to_string(line) + "\x01" +
           std::to_string(col) + "\x01" + std::to_string(len) + "\x01" + msg;
}
static bool aeUnpack(const std::string& what, int& file, int& line, int& col, int& len, std::string& msg) {
    if (what.empty() || what[0] != '\x01') return false;
    size_t p = 1;
    int* out[4] = { &file, &line, &col, &len };
    for (int i = 0; i < 4; i++) {
        size_t q = what.find('\x01', p);
        if (q == std::string::npos) return false;
        *out[i] = atoi(what.substr(p, q - p).c_str());
        p = q + 1;
    }
    msg = what.substr(p);
    return true;
}

// 把 Unicode 码点编码成 UTF-8（供 \uXXXX 转义使用）
static string utf8Encode(unsigned cp) {
    string s;
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) {
        s += (char)(0xC0 | (cp >> 6));
        s += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += (char)(0xE0 | (cp >> 12));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    } else {
        s += (char)(0xF0 | (cp >> 18));
        s += (char)(0x80 | ((cp >> 12) & 0x3F));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    }
    return s;
}

// 字节码写入辅助（大端）
static void writeU16(vector<uint8_t>& v, uint16_t val) {
    v.push_back(static_cast<uint8_t>((val >> 8) & 0xFF)); v.push_back(static_cast<uint8_t>(val & 0xFF));
}
static void writeU32(vector<uint8_t>& v, uint32_t val) {
    v.push_back(static_cast<uint8_t>((val >> 24) & 0xFF)); v.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((val >> 8) & 0xFF)); v.push_back(static_cast<uint8_t>(val & 0xFF));
}
