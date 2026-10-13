#pragma once
// 常量池与字节码缓冲区
// 常量池是前端与两个后端共用的数据契约：字节码后端把它序列化成常量段，
// AOT 后端拿它生成 C 的静态数据。
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include "ae_common.h"

// 常量池（ 新增 NULL 类型 tag）
struct Const {
    enum { UTF8 = 1, INT32 = 2, DOUBLE = 3, NULLV = 4, INT64 = 5 } tag;   // NULLV / INT64
    string  s;
    int32_t i;
    int64_t i64 = 0;    // P0-2
    double  d;
};

class ConstantPool {
public:
    vector<Const> pool;

    uint16_t addNull() {
        for (uint16_t k = 0; k < (uint16_t)pool.size(); k++)
            if (pool[k].tag == Const::NULLV) return k;
        Const c; c.tag = Const::NULLV; pool.push_back(c); return (uint16_t)pool.size() - 1;
    }
    uint16_t addString(const string& s) {
        for (uint16_t k = 0; k < (uint16_t)pool.size(); k++)
            if (pool[k].tag == Const::UTF8 && pool[k].s == s) return k;
        Const c; c.tag = Const::UTF8; c.s = s; pool.push_back(c); return (uint16_t)pool.size() - 1;
    }
    uint16_t addInt(int32_t v) {
        for (uint16_t k = 0; k < (uint16_t)pool.size(); k++)
            if (pool[k].tag == Const::INT32 && pool[k].i == v) return k;
        Const c; c.tag = Const::INT32; c.i = v; pool.push_back(c); return (uint16_t)pool.size() - 1;
    }
    // P0-2：运行期是 int64，常量池必须能装下整个 int64
    uint16_t addInt64(int64_t v) {
        for (uint16_t k = 0; k < (uint16_t)pool.size(); k++)
            if (pool[k].tag == Const::INT64 && pool[k].i64 == v) return k;
        Const c; c.tag = Const::INT64; c.i64 = v; pool.push_back(c); return (uint16_t)pool.size() - 1;
    }
    // 小整数仍用 INT32（字节码更紧凑，也与既有 .aeo 保持兼容）
    uint16_t addIntAuto(int64_t v) {
        if (v >= INT32_MIN && v <= INT32_MAX) return addInt((int32_t)v);
        return addInt64(v);
    }
    uint16_t addDouble(double v) {
        for (uint16_t k = 0; k < (uint16_t)pool.size(); k++)
            if (pool[k].tag == Const::DOUBLE && pool[k].d == v) return k;
        Const c; c.tag = Const::DOUBLE; c.d = v; pool.push_back(c); return (uint16_t)pool.size() - 1;
    }

    void write(vector<uint8_t>& out) const {
        writeU32(out, (uint32_t)pool.size());
        for (const Const& c : pool) {
            out.push_back((uint8_t)c.tag);
            if (c.tag == Const::UTF8) {
                writeU32(out, (uint32_t)c.s.size());
                out.insert(out.end(), c.s.begin(), c.s.end());
            } else if (c.tag == Const::INT32) {
                writeU32(out, (uint32_t)(uint32_t)c.i);
            } else if (c.tag == Const::INT64) {
                uint64_t bits = (uint64_t)c.i64;
                for (int s = 56; s >= 0; s -= 8) out.push_back((uint8_t)((bits >> s) & 0xFF));
            } else if (c.tag == Const::DOUBLE) {
                uint64_t bits; memcpy(&bits, &c.d, 8);
                for (int s = 56; s >= 0; s -= 8) out.push_back((uint8_t)((bits >> s) & 0xFF));
            }
            // NULLV：仅 tag 字节，无附加数据
        }
    }
};

// 字节码缓冲区
struct CodeBuf {
    vector<uint8_t> code;
    // 指令偏移 → (源码行, 列)：运行期错误据此定位到具体位置
    struct LineEntry { uint32_t off, line, col; uint16_t file; };   // file：模块编译后不止一个源文件
    vector<LineEntry> lines;
    uint32_t pendingLine = 1, pendingCol = 1;
    uint16_t pendingFile = 0;                    // advance() 更新，emit() 记录
    void markPos(uint32_t line, uint32_t col) {
        if (!lines.empty() && lines.back().line == line && lines.back().col == col &&
            lines.back().file == pendingFile) return;
        lines.push_back({(uint32_t)code.size(), line, col, pendingFile});
    }
    void markLine(uint32_t line) { markPos(line, 1); }
    void emit(uint8_t b) {
        markPos(pendingLine, pendingCol);
        code.push_back(b);
    }
    void u8(uint8_t v)  { code.push_back(v); }
    void u16(uint16_t v){ writeU16(code, v); }
    void u32(uint32_t v){ writeU32(code, v); }
    size_t size() const { return code.size(); }
    void patchU16At(size_t pos, uint16_t val) {
        code[pos]   = static_cast<uint8_t>((val >> 8) & 0xFF);
        code[pos+1] = static_cast<uint8_t>(val & 0xFF);
    }
    void patchU8At(size_t pos, uint8_t val) { code[pos] = val; }   // P1-5
    void write(vector<uint8_t>& out) const {
        writeU32(out, (uint32_t)code.size());
        out.insert(out.end(), code.begin(), code.end());
    }
};
