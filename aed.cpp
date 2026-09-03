// =============================================================================
//  Ae 反汇编器 (aed = Ae Disassembler)
// =============================================================================

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <iomanip>

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
        default: return "?";
    }
}

int main(int argc, char** argv) {
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

    // 常量池
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
        } else {
            cout << "unknown(tag=0x" << hex << (int)tag << dec << ")";
        }
        cout << endl;
    }
    cout << endl;

    // 代码
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
            case 0x02: { uint8_t fid = readU8(all, p); uint8_t argc = readU8(all, p);
                         cout << "CALL       " << funcName(fid) << "  argc=" << (int)argc; break; }
            case 0x03: cout << "HALT"; break;
            case 0x04: { uint16_t s = readU16(all, p); cout << "STORE      slot@" << s; break; }
            case 0x05: { uint16_t s = readU16(all, p); cout << "LOAD_VAR   slot@" << s; break; }
            case 0x06: cout << "IADD"; break;
            case 0x07: cout << "ISUB"; break;
            case 0x08: cout << "IMUL"; break;
            case 0x09: cout << "IDIV"; break;
            case 0x0A: cout << "FADD   ; 浮点加 / 字符串拼接"; break;
            case 0x0B: cout << "FSUB"; break;
            case 0x0C: cout << "FMUL"; break;
            case 0x0D: cout << "FDIV"; break;
            case 0x0E: cout << "ITOD"; break;
            case 0x0F: cout << "IMOD"; break;
            case 0x10: cout << "IEQ    ; =="; break;
            case 0x11: cout << "INEQ   ; !="; break;
            case 0x12: cout << "ILT    ; <"; break;
            case 0x13: cout << "ILE    ; <="; break;
            case 0x14: cout << "IGT    ; >"; break;
            case 0x15: cout << "IGE    ; >="; break;
            case 0x16: cout << "FCMP_EQ ; =="; break;
            case 0x17: cout << "FCMP_NE ; !="; break;
            case 0x18: cout << "FCMP_LT ; <"; break;
            case 0x19: cout << "FCMP_LE ; <="; break;
            case 0x1A: cout << "FCMP_GT ; >"; break;
            case 0x1B: cout << "FCMP_GE ; >="; break;
            case 0x2A: { uint16_t off = readU16(all, p);
                        cout << "JZ         +" << off << "  ; 条件为假则跳"; break; }
            case 0x2B: { uint16_t off = readU16(all, p);
                        cout << "JMP        +" << off; break; }
            case 0x30: { uint16_t s = readU16(all, p); cout << "LOCAL_STORE slot@" << s; break; }
            case 0x31: { uint16_t s = readU16(all, p); cout << "LOCAL_LOAD  slot@" << s; break; }
            case 0x40: { uint16_t fid = readU16(all, p);
                         uint16_t paramCount = readU16(all, p);
                         uint16_t lc = readU16(all, p);
                         uint32_t cs = readU32(all, p);
                         cout << "FUNC_DEF   fid=" << fid << " params=" << paramCount << " locals=" << lc << " codesize=" << cs; break; }
            case 0x41: { uint16_t fid = readU16(all, p); uint8_t argc = readU8(all, p);
                         cout << "CALL_FUNC  fid=" << fid << " argc=" << (int)argc; break; }
            case 0x42: cout << "RET"; break;
            case 0x43: cout << "POP"; break;
            default:    cout << "UNK(0x" << hex << (int)op << dec << ")"; break;
        }
        cout << endl;
    }
    cout << endl;
    cout << "; ── End of Disassembly ─────────────────────────────────────" << endl;
    return 0;
}