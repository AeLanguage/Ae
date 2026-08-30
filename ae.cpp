// =============================================================================
//  Ae 虚拟机 (ae = Ae VM)
// -----------------------------------------------------------------------------
//  读取 .aeo 字节码 → 解析 → 执行
//
//  ▌运行时值模型 (Tagged Value / 类型化栈)
//  ─────────────────────────────────────────────────────────────────────────
//  每个运行时值 = payload + 类型标签，存于操作数栈。
//  类型用独立的"类型栈"跟踪，与值栈平行 —— Lua/CPython 的标准做法。
//
//  ValueKind:
//      VAL_INT    = 0   → int64 payload
//      VAL_DOUBLE = 1   → double payload
//      VAL_STR    = 2   → uint16 常量池索引 payload
//      VAL_BOOL   = 3   → int64 payload (0=false, 1=true)
// =============================================================================

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <sstream>

using namespace std;

// -----------------------------------------------------------------------------
//  常量池条目
// -----------------------------------------------------------------------------
struct Const {
    enum { UTF8 = 1, INT32 = 2, DOUBLE = 3 } tag;
    string   s;
    int32_t  i = 0;
    double   d = 0;
};

// -----------------------------------------------------------------------------
//  运行时值
// -----------------------------------------------------------------------------
enum ValueKind { VAL_INT = 0, VAL_DOUBLE = 1, VAL_STR = 2, VAL_BOOL = 3 };

struct Value {
    ValueKind kind;
    union {
        int64_t  i;
        double   f;
        uint16_t strIdx;
    };
    Value() : kind(VAL_INT), i(0) {}
    Value(int64_t v) : kind(VAL_INT), i(v) {}
    Value(double v)  : kind(VAL_DOUBLE), f(v) {}
    Value(ValueKind k, uint16_t idx) : kind(k), strIdx(idx) {}
    Value(bool b)    : kind(VAL_BOOL), i(b ? 1 : 0) {}
};

// -----------------------------------------------------------------------------
//  字节码读取辅助（大端）
// -----------------------------------------------------------------------------
static uint8_t  readU8 (const vector<uint8_t>& b, size_t& p) { return b[p++]; }
static uint16_t readU16(const vector<uint8_t>& b, size_t& p) {
    uint16_t v = (uint16_t)((b[p] << 8) | b[p+1]); p += 2; return v;
}
static uint32_t readU32(const vector<uint8_t>& b, size_t& p) {
    uint32_t v = (uint32_t)((b[p] << 24) | (b[p+1] << 16) | (b[p+2] << 8) | b[p+3]); p += 4; return v;
}

// =============================================================================
//  虚拟机
// =============================================================================
class VM {
public:
    vector<Const> constPool;
    vector<uint8_t> code;
    size_t pc = 0;

    vector<Value> stack;
    vector<Value> globals;

    // ---- 栈操作 ----
    Value pop() {
        if (stack.empty()) throw runtime_error("栈下溢");
        Value v = stack.back(); stack.pop_back(); return v;
    }
    void push(const Value& v) { stack.push_back(v); }

    double toDouble(const Value& v) {
        if (v.kind == VAL_DOUBLE) return v.f;
        if (v.kind == VAL_INT)    return (double)v.i;
        if (v.kind == VAL_BOOL)   return (double)v.i;
        return 0.0;
    }

    bool isFalse(const Value& v) const {
        if (v.kind == VAL_BOOL)   return v.i == 0;
        if (v.kind == VAL_INT)    return v.i == 0;
        if (v.kind == VAL_DOUBLE) return v.f == 0.0;
        return false; // string 非空视为 true
    }

    void run() {
        while (pc < code.size()) {
            uint8_t op = code[pc++];
            switch (op) {
                case 0x00: // NOP
                    break;

                case 0x01: { // LOAD_CONST u16(idx)
                    uint16_t idx = readU16(code, pc);
                    const Const& c = constPool[idx];
                    if (c.tag == Const::UTF8) {
                        push(Value(VAL_STR, idx));
                    } else if (c.tag == Const::INT32) {
                        push(Value((int64_t)c.i));
                    } else if (c.tag == Const::DOUBLE) {
                        push(Value(c.d));
                    }
                    break;
                }

                case 0x02: { // CALL u8(funcId) u8(argc)
                    uint8_t funcId = readU8(code, pc);
                    uint8_t argc   = readU8(code, pc);
                    invoke(funcId, argc);
                    break;
                }

                case 0x03: // HALT
                    return;

                case 0x04: { // STORE u16(slot)
                    uint16_t slot = readU16(code, pc);
                    if (slot >= globals.size()) globals.resize(slot + 1);
                    globals[slot] = pop();
                    break;
                }

                case 0x05: { // LOAD_VAR u16(slot)
                    uint16_t slot = readU16(code, pc);
                    if (slot >= globals.size()) globals.resize(slot + 1);
                    push(globals[slot]);
                    break;
                }

                // ---- 算术指令 ----
                case 0x06: { Value r = pop(), l = pop(); push(Value(l.i + r.i)); break; } // IADD
                case 0x07: { Value r = pop(), l = pop(); push(Value(l.i - r.i)); break; } // ISUB
                case 0x08: { Value r = pop(), l = pop(); push(Value(l.i * r.i)); break; } // IMUL
                case 0x09: { // IDIV
                    Value r = pop(), l = pop();
                    if (r.i == 0) throw runtime_error("整数除以零");
                    push(Value(l.i / r.i));
                    break;
                }
                case 0x0A: { Value r = pop(), l = pop(); push(Value(toDouble(l) + toDouble(r))); break; } // FADD
                case 0x0B: { Value r = pop(), l = pop(); push(Value(toDouble(l) - toDouble(r))); break; } // FSUB
                case 0x0C: { Value r = pop(), l = pop(); push(Value(toDouble(l) * toDouble(r))); break; } // FMUL
                case 0x0D: { // FDIV
                    Value r = pop(), l = pop();
                    double rd = toDouble(r);
                    if (rd == 0.0) throw runtime_error("浮点除以零");
                    push(Value(toDouble(l) / rd));
                    break;
                }
                case 0x0E: { Value v = pop(); push(Value((double)toDouble(v))); break; } // ITOD
                case 0x0F: { // IMOD
                    Value r = pop(), l = pop();
                    if (r.i == 0) throw runtime_error("取模除以零");
                    push(Value((int64_t)(l.i % r.i)));
                    break;
                }

                // ---- 整数比较 (0x10-0x15) → VAL_BOOL ----
                case 0x10: { Value r = pop(), l = pop(); push(Value((bool)(l.i == r.i))); break; }
                case 0x11: { Value r = pop(), l = pop(); push(Value((bool)(l.i != r.i))); break; }
                case 0x12: { Value r = pop(), l = pop(); push(Value((bool)(l.i <  r.i))); break; }
                case 0x13: { Value r = pop(), l = pop(); push(Value((bool)(l.i <= r.i))); break; }
                case 0x14: { Value r = pop(), l = pop(); push(Value((bool)(l.i >  r.i))); break; }
                case 0x15: { Value r = pop(), l = pop(); push(Value((bool)(l.i >= r.i))); break; }

                // ---- 浮点比较 (0x16-0x1B) → VAL_BOOL ----
                case 0x16: { Value r = pop(), l = pop(); push(Value((bool)(toDouble(l) == toDouble(r)))); break; }
                case 0x17: { Value r = pop(), l = pop(); push(Value((bool)(toDouble(l) != toDouble(r)))); break; }
                case 0x18: { Value r = pop(), l = pop(); push(Value((bool)(toDouble(l) <  toDouble(r)))); break; }
                case 0x19: { Value r = pop(), l = pop(); push(Value((bool)(toDouble(l) <= toDouble(r)))); break; }
                case 0x1A: { Value r = pop(), l = pop(); push(Value((bool)(toDouble(l) >  toDouble(r)))); break; }
                case 0x1B: { Value r = pop(), l = pop(); push(Value((bool)(toDouble(l) >= toDouble(r)))); break; }

                // ---- 控制流 ----
                case 0x2A: { // JZ  u16(offset)  栈顶为 false/0 时跳转（有符号偏移）
                    uint16_t u = readU16(code, pc);
                    Value v = pop();
                    if (isFalse(v)) pc = (size_t)((int64_t)pc + (int16_t)u);
                    break;
                }
                case 0x2B: { // JMP u16(offset)  无条件跳转（有符号偏移）
                    uint16_t u = readU16(code, pc);
                    pc = (size_t)((int64_t)pc + (int16_t)u);
                    break;
                }

                default:
                    throw runtime_error("未知指令: 0x" + to_string(op));
            }
        }
    }

    void invoke(uint8_t funcId, uint8_t argc) {
        if (funcId == 0) {
            // pr(...)
            size_t base = stack.size() - argc;
            for (size_t i = 0; i < argc; i++) {
                Value v = stack[base + i];
                if (i > 0) cout << " ";
                printValue(v);
            }
            cout << endl;
            for (uint8_t i = 0; i < argc; i++) stack.pop_back();
        } else {
            throw runtime_error("调用未定义的函数 id=" + to_string(funcId));
        }
    }

    void printValue(const Value& v) {
        if (v.kind == VAL_STR) {
            cout << constPool[v.strIdx].s;
        } else if (v.kind == VAL_BOOL) {
            cout << (v.i ? "true" : "false");
        } else if (v.kind == VAL_DOUBLE) {
            if (v.f == (double)(int64_t)v.f) {
                cout << (int64_t)v.f;
            } else {
                cout << v.f;
            }
        } else { // VAL_INT
            cout << v.i;
        }
    }

    // ---- 加载字节码 ----
    void load(const string& path) {
        ifstream f(path, ios::binary);
        if (!f) throw runtime_error("无法打开: " + path);
        vector<uint8_t> all((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
        if (all.size() < 16) throw runtime_error("文件过短，不是有效的 .aeo 文件");

        size_t p = 0;
        if (all[p] != 'A' || all[p+1] != 'e' || all[p+2] != 'B' || all[p+3] != 'c')
            throw runtime_error("魔数不匹配，不是有效的 .aeo 文件");
        p += 4;
        uint8_t major = all[p++], minor = all[p++];
        p += 2; // flags
        (void)major; (void)minor;

        // 常量池
        uint32_t poolCount = readU32(all, p);
        constPool.resize(poolCount);
        for (uint32_t i = 0; i < poolCount; i++) {
            uint8_t tag = readU8(all, p);
            constPool[i].tag = (decltype(Const::tag))tag;
            if (tag == Const::UTF8) {
                uint32_t len = readU32(all, p);
                constPool[i].s = string((const char*)(all.data() + p), len);
                p += len;
            } else if (tag == Const::INT32) {
                int32_t v = (int32_t)readU32(all, p);
                constPool[i].i = v;
            } else if (tag == Const::DOUBLE) {
                uint64_t bits = 0;
                for (int s = 56; s >= 0; s -= 8) bits = (bits << 8) | readU8(all, p);
                memcpy(&constPool[i].d, &bits, 8);
            }
        }

        // 代码
        uint32_t codeLen = readU32(all, p);
        code.assign(all.begin() + p, all.begin() + p + codeLen);
    }
};

// =============================================================================
//  main
// =============================================================================
int main(int argc, char** argv) {
    string path;
    if (argc > 1) {
        path = argv[1];
    } else {
        cout << "Ae 虚拟机 (ae) - 输入字节码文件路径: ";
        getline(cin, path);
        if (path.empty()) { cerr << "错误: 未提供输入文件" << endl; return 1; }
    }
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
        path = path.substr(1, path.size() - 2);

    try {
        VM vm;
        vm.load(path);
        vm.run();
    } catch (const exception& e) {
        cerr << "✗ 运行错误: " << e.what() << endl;
        return 1;
    }
    return 0;
}