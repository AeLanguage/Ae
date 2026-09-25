// =============================================================================
//  Ae 反汇编器 (aed = Ae Disassembler)  ——  通用表指令 + 严格比较族 + 布尔常量
// =============================================================================

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include "ae_console.h"     // ★ 控制台中文：绕开代码页

using namespace std;

static uint8_t  readU8 (const vector<uint8_t>& b, size_t& p) { return b[p++]; }
static uint16_t readU16(const vector<uint8_t>& b, size_t& p) { uint16_t v=(uint16_t)((b[p]<<8)|b[p+1]); p+=2; return v; }
static uint32_t readU32(const vector<uint8_t>& b, size_t& p) { uint32_t v=(uint32_t)((b[p]<<24)|(b[p+1]<<16)|(b[p+2]<<8)|b[p+3]); p+=4; return v; }

static const char* funcName(uint8_t id) {
    switch (id) {
        case 0: return "pr";
        case 1: return "inp";
        case 2: return "prln";
        case 3: return "len";
        case 4: return "str";
        case 5: return "int";
        case 6: return "float";
        case 7: return "type";     // ★
        case 8: return "assert";   // ★ P1-6
        case 9: return "error";    // ★ P1-6
        case 10: return "exit";    // ★ P1-6
        // ★ P2-11/12/14/16
        case 11: return "substr";
        case 12: return "ord";
        case 13: return "chr";
        case 14: return "find";
        case 15: return "upper";
        case 16: return "lower";
        case 17: return "trim";
        case 18: return "replace";
        case 19: return "split";
        case 20: return "join";
        case 21: return "repeat";
        case 22: return "format";
        case 23: return "delete";
        case 24: return "insert";
        case 25: return "pop";
        case 26: return "append";
        case 27: return "count";
        case 28: return "copy";
        case 29: return "deepcopy";
        case 30: return "equal";
        default: return "?";
    }
}

int main(int argc, char** argv) {
    aecon::install();    // ★ 第一件事：把 cout/cerr/cin 接到控制台（GBK 的 cmd 里中文不乱码）
    string path;
    if (argc > 1) path = argv[1];
    else {
        cout << "Ae 反汇编器 (aed) - 输入字节码文件路径: ";
        getline(cin, path);
        if (path.empty()) { cerr << "错误: 未提供输入文件" << endl; return 1; }
    }
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
        path = path.substr(1, path.size() - 2);

    ifstream f(path, ios::binary);
    if (!f) { cerr << "错误: 无法打开 '" << path << "'" << endl; return 1; }
    vector<uint8_t> all((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());

    if (all.size() < 16) { cerr << "错误: 文件过短" << endl; return 1; }
    if (all[0]!='A'||all[1]!='e'||all[2]!='B'||all[3]!='c') { cerr << "错误: 魔数不匹配" << endl; return 1; }

    size_t p = 4;
    uint8_t major = all[p++], minor = all[p++];
    (void)readU16(all, p); // flags
    uint32_t poolCount = readU32(all, p);

    cout << "; ╔══════════════════════════════════════════════════════════╗" << endl;
    cout << "; ║  Ae Bytecode Disassembly  (" << path << ")" << endl;
    cout << "; ╠══════════════════════════════════════════════════════════╣" << endl;
    cout << "; ║  Magic: AeBc  Version: " << (int)major << "." << (int)minor << endl;
    cout << "; ╚══════════════════════════════════════════════════════════╝" << endl;
    cout << endl;

    cout << "; ── Constant Pool (" << poolCount << " entries) ───────────────" << endl;
    for (uint32_t i = 0; i < poolCount; i++) {
        uint8_t tag = readU8(all, p);
        cout << "  #" << setw(3) << i << "  ";
        if (tag == 0x01) {
            uint32_t len = readU32(all, p);
            string s((const char*)(all.data() + p), len); p += len;
            cout << "utf8   \"" << s << "\"";
        } else if (tag == 0x02) {
            int32_t v = (int32_t)readU32(all, p);
            cout << "int32  " << v;
        } else if (tag == 0x03) {
            uint64_t bits = 0;
            for (int s = 56; s >= 0; s -= 8) bits = (bits << 8) | readU8(all, p);
            double d; memcpy(&d, &bits, 8);
            cout << "double " << d;
        } else if (tag == 0x04) {                                 // ★ null
            cout << "null";
        } else if (tag == 0x05) {                                 // ★ P0-2 int64
            uint64_t bits = 0;
            for (int s = 56; s >= 0; s -= 8) bits = (bits << 8) | readU8(all, p);
            int64_t v; memcpy(&v, &bits, 8);
            cout << "int64  " << v;
        } else {
            cout << "unknown(tag=0x" << hex << (int)tag << dec << ")";
        }
        cout << endl;
    }
    cout << endl;

    // ★ P1-1/P1-2：v1.1 的调试信息段
    string srcName;
    vector<pair<uint16_t,string>> funcNames;
    vector<pair<uint32_t,uint32_t>> lineTable;
    if (minor >= 1) {
        uint32_t n = readU32(all, p);
        srcName.assign((const char*)(all.data() + p), n); p += n;
        if (minor >= 6) {
            uint32_t fc = readU32(all, p);
            for (uint32_t i = 0; i < fc; i++) {
                uint32_t len = readU32(all, p); p += len;      // 源文件表（跳过）
            }
            cout << "; 源文件数: " << fc << endl;
        }
        uint32_t nameCount = readU32(all, p);
        for (uint32_t i = 0; i < nameCount; i++) {
            uint16_t fid = readU16(all, p);
            uint32_t len = readU32(all, p);
            string nm((const char*)(all.data() + p), len); p += len;
            funcNames.push_back({fid, nm});
        }
        uint32_t lineCount = readU32(all, p);
        for (uint32_t i = 0; i < lineCount; i++) {
            uint32_t off = readU32(all, p);
            uint32_t ln  = readU32(all, p);
            uint32_t cl  = (minor >= 4) ? readU32(all, p) : 1;   // 1.4 起带列号
            uint32_t fi  = (minor >= 6) ? readU16(all, p) : 0;   // 1.6 起带文件索引
            if (i < 8) cout << ";   偏移 " << off << " -> 文件#" << fi << " 第 " << ln << " 行 第 " << cl << " 列" << endl;
            lineTable.push_back({off, ln});
        }
        cout << "; ── Debug Info (v1.1) ──────────────────────────" << endl;
        cout << "; 源文件: " << srcName << endl;
        cout << "; 函数名表 (" << funcNames.size() << ")：" << endl;
        for (const auto& fn : funcNames)
            cout << ";   fid=" << fn.first << "  " << fn.second << endl;
        cout << "; 行号表 (" << lineTable.size() << " 条)" << endl;
        cout << endl;
    }

    uint32_t codeLen = readU32(all, p);
    cout << "; ── Code (" << codeLen << " bytes) ────────────────────────────" << endl;
    size_t start = p;
    while (p < all.size() && (p - start) < codeLen) {
        size_t instrPos = p;
        uint8_t op = all[p++];
        cout << "  " << setw(4) << instrPos - start << ": ";

        switch (op) {
            case 0x00: cout << "NOP"; break;
            case 0x01: { uint16_t i = readU16(all, p); cout << "LOAD_CONST  #" << i; break; }
            case 0x02: { uint8_t fid = readU8(all, p); uint8_t argc = readU8(all, p); uint8_t want = readU8(all, p);
                         cout << "CALL       " << funcName(fid) << "  argc=" << (int)argc
                              << " want=" << (int)want; break; }
            case 0x03: cout << "HALT"; break;
            case 0x04: { uint16_t s = readU16(all, p); cout << "STORE      slot@" << s; break; }
            case 0x05: { uint16_t s = readU16(all, p); cout << "LOAD_VAR   slot@" << s; break; }
            case 0x06: cout << "IADD"; break;
            case 0x07: cout << "ISUB"; break;
            case 0x08: cout << "IMUL"; break;
            case 0x09: cout << "IDIV"; break;
            case 0x0A: cout << "ADD    ; 整数加 / 浮点加 / 字符串拼接"; break;
            case 0x0B: cout << "SUB"; break;
            case 0x0C: cout << "MUL"; break;
            case 0x0D: cout << "DIV"; break;
            case 0x0E: cout << "ITOD"; break;
            case 0x0F: cout << "IMOD"; break;
            // ★ 严格比较：整数族(0x10-0x15) / 浮点族(0x16-0x1B) 行为等价，
            //   均由 VM 按运行期类型判定
            case 0x10: cout << "CMP_EQ  ; =="; break;
            case 0x11: cout << "CMP_NEQ ; !="; break;
            case 0x12: cout << "CMP_LT  ; <"; break;
            case 0x13: cout << "CMP_LE  ; <="; break;
            case 0x14: cout << "CMP_GT  ; >"; break;
            case 0x15: cout << "CMP_GE  ; >="; break;
            case 0x16: cout << "FCMP_EQ ; =="; break;
            case 0x17: cout << "FCMP_NEQ; !="; break;
            case 0x18: cout << "FCMP_LT ; <"; break;
            case 0x19: cout << "FCMP_LE ; <="; break;
            case 0x1A: cout << "FCMP_GT ; >"; break;
            case 0x1B: cout << "FCMP_GE ; >="; break;
            // ★ 布尔常量
            // ★ P2-1 / P2-2：位运算与幂
            case 0x1F: cout << "BNOT ; ~"; break;
            case 0x20: cout << "BAND ; &"; break;
            case 0x21: cout << "BOR  ; |"; break;
            case 0x22: cout << "BXOR ; ^"; break;
            case 0x23: cout << "SHL  ; <<"; break;
            case 0x24: cout << "SHR  ; >> (算术右移)"; break;
            case 0x25: cout << "POW  ; **"; break;
            // ★ P2-1：位运算
            case 0x1C: cout << "LOAD_TRUE"; break;
            case 0x1D: cout << "LOAD_FALSE"; break;
            case 0x1E: cout << "NOT"; break;
            // 通用表指令（0x50=建表, 0x51=GET, 0x52=SET，与 VM 一致）
            case 0x50: { uint16_t n = readU16(all, p); cout << "NEW_TABLE  count=" << n; break; }
            case 0x51: cout << "TABLE_GET  ; table, key → value（未定义得 null）"; break;
            case 0x52: cout << "TABLE_SET  ; table, key, val → 就地修改"; break;
            case 0x53: cout << "TABLE_KEYS ; table → 键数组（顺序确定）"; break;
            case 0x54: cout << "TABLE_LEN  ; table → 长度"; break;
            case 0x55: { uint16_t i = readU16(all, p); cout << "THROW_CONST #" << i; break; }
            // ★ 错误捕获：处理器注册 / 注销
            case 0x27: { uint16_t off = readU16(all, p); cout << "PUSH_HANDLER +" << off; break; }
            case 0x28: cout << "POP_HANDLER"; break;
            case 0x2A: { uint16_t off = readU16(all, p); cout << "JZ         +" << off; break; }

            case 0x2B: { uint16_t off = readU16(all, p); cout << "JMP        +" << off; break; }
            case 0x30: { uint16_t s = readU16(all, p); cout << "LOCAL_STORE slot@" << s; break; }
            case 0x31: { uint16_t s = readU16(all, p); cout << "LOCAL_LOAD  slot@" << s; break; }
            case 0x40: { uint16_t fid = readU16(all, p);
                         uint16_t paramCount = readU16(all, p);
                         uint16_t lc = readU16(all, p);
                         uint32_t cs = readU32(all, p);
                         cout << "FUNC_DEF   fid=" << fid << " params=" << paramCount << " locals=" << lc << " codesize=" << cs; break; }
            case 0x41: { uint16_t fid = readU16(all, p); uint8_t argc = readU8(all, p); uint8_t want = readU8(all, p);
                         cout << "CALL_FUNC  fid=" << fid << " argc=" << (int)argc
                              << " want=" << (int)want; break; }
            case 0x42: { uint8_t n = readU8(all, p); cout << "RET        n=" << (int)n; break; }
            case 0x43: cout << "POP"; break;
            default:    cout << "UNK(0x" << hex << (int)op << dec << ")"; break;
        }
        cout << endl;
    }
    cout << endl;
    cout << "; ── End of Disassembly ─────────────────────────────────────" << endl;
    return 0;
}