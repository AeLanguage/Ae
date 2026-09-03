// =============================================================================
//  Ae 虚拟机 (ae = Ae VM)  ——  支持自定义函数 + 局部变量 + 递归
//                            + 字符串拼接 + len/str/int/float 内置函数
// =============================================================================

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <memory>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <iomanip>

using namespace std;

// 字节码读取辅助（大端）
static uint8_t  readU8 (const vector<uint8_t>& b, size_t& p) { return b[p++]; }
static uint16_t readU16(const vector<uint8_t>& b, size_t& p) {
    uint16_t v = (uint16_t)((b[p] << 8) | b[p+1]); p += 2; return v;
}
static uint32_t readU32(const vector<uint8_t>& b, size_t& p) {
    uint32_t v = (uint32_t)((b[p] << 24) | (b[p+1] << 16) | (b[p+2] << 8) | b[p+3]); p += 4; return v;
}

// 常量池条目
struct Const {
    enum { UTF8 = 1, INT32 = 2, DOUBLE = 3 } tag;
    string   s;
    int32_t  i = 0;
    double   d = 0;
};

// 运行时值
enum ValueKind { VAL_INT = 0, VAL_DOUBLE = 1, VAL_STR = 2, VAL_BOOL = 3 };

struct Value {
    ValueKind kind;
    union {
        int64_t  i;
        double   f;
    };
    shared_ptr<string> str;   // 仅 VAL_STR

    Value() : kind(VAL_INT), i(0) {}
    Value(int64_t v) : kind(VAL_INT), i(v) {}
    Value(double v)  : kind(VAL_DOUBLE), f(v) {}
    explicit Value(shared_ptr<string> s) : kind(VAL_STR), str(s) {}
    Value(bool b)    : kind(VAL_BOOL), i(b ? 1 : 0) {}
    Value(const string& s) : kind(VAL_STR), str(make_shared<string>(s)) {}
};

// 函数定义（用户自定义）
struct FuncDef {
    uint16_t funcId;       // 函数索引
    uint16_t paramCount;   // 参数个数
    uint16_t localCount;   // 局部变量个数（含参数）
    uint32_t entryPC;      // 函数入口字节码偏移
    uint32_t codeSize;     // 函数体字节码大小
};

// 调用帧（用于递归调用栈）
struct CallFrame {
    uint16_t funcId;       // 当前函数 ID
    size_t   returnPC;     // 返回地址（CALL_FUNC 之后的 pc）
    vector<Value> locals;  // 局部变量区 [param0, param1, ..., local0, local1, ...]
};

// =============================================================================
//  虚拟机
// =============================================================================
class VM {
public:
    vector<Const> constPool;
    vector<uint8_t> code;
    size_t pc = 0;

    vector<Value> stack;         // 操作数栈
    vector<Value> globals;       // 全局变量
    vector<FuncDef> funcs;       // 函数表

    vector<CallFrame> callStack; // 调用栈

    // 当前帧的局部变量（nullptr = 顶层/main 代码，只用全局）
    vector<Value>* currentLocals = nullptr;

    Value pop() {
        if (stack.empty()) throw runtime_error("栈下溢");
        Value v = stack.back(); stack.pop_back(); return v;
    }
    void push(const Value& v) { stack.push_back(v); }

    double toDouble(const Value& v) {
        if (v.kind == VAL_DOUBLE) return v.f;
        if (v.kind == VAL_INT)    return (double)v.i;
        if (v.kind == VAL_BOOL)   return (double)v.i;
        if (v.kind == VAL_STR) {
            if (!v.str || v.str->empty()) return 0.0;
            try { return stod(*v.str); } catch (...) { return 0.0; }
        }
        return 0.0;
    }

    // 将任意值转为字符串（用于字符串拼接 / str() 函数）
    string toString(const Value& v) {
        if (v.kind == VAL_STR) {
            return v.str ? *v.str : "";
        } else if (v.kind == VAL_BOOL) {
            return v.i ? "true" : "false";
        } else if (v.kind == VAL_DOUBLE) {
            ostringstream oss;
            oss << fixed << setprecision(6) << v.f;
            string s = oss.str();
            // 去掉末尾多余的 0
            size_t dot = s.find('.');
            if (dot != string::npos) {
                while (s.size() > dot + 2 && s.back() == '0') s.pop_back();
                if (s.back() == '.') s.pop_back();
            }
            return s;
        } else {
            return to_string(v.i);
        }
    }

    int64_t toInt(const Value& v) {
        if (v.kind == VAL_INT)    return v.i;
        if (v.kind == VAL_DOUBLE) return (int64_t)v.f;
        if (v.kind == VAL_BOOL)   return v.i;
        if (v.kind == VAL_STR) {
            if (!v.str || v.str->empty()) return 0;
            try { return stoll(*v.str); } catch (...) { return 0; }
        }
        return 0;
    }

    bool isFalse(const Value& v) const {
        if (v.kind == VAL_BOOL)   return v.i == 0;
        if (v.kind == VAL_INT)    return v.i == 0;
        if (v.kind == VAL_DOUBLE) return v.f == 0.0;
        if (v.kind == VAL_STR)    return v.str == nullptr || *v.str == "";
        return false;
    }

    // 预处理：扫描所有 FUNC_DEF 指令，建立函数表
    void preScanFunctions() {
        size_t savedPC = pc;
        pc = 0;
        while (pc < code.size()) {
            uint8_t op = code[pc++];
            if (op == 0x40) { // FUNC_DEF
                if (pc + 11 > code.size()) break;
                uint16_t funcId    = (uint16_t)((code[pc] << 8) | code[pc+1]); pc += 2;
                uint16_t paramCount = (uint16_t)((code[pc] << 8) | code[pc+1]); pc += 2;
                uint16_t localCount = (uint16_t)((code[pc] << 8) | code[pc+1]); pc += 2;
                uint32_t codeSize  = (uint32_t)((code[pc] << 24) | (code[pc+1] << 16) | (code[pc+2] << 8) | code[pc+3]); pc += 4;
                FuncDef fd;
                fd.funcId     = funcId;
                fd.paramCount = paramCount;
                fd.localCount = localCount;
                fd.entryPC    = (uint32_t)pc;
                fd.codeSize   = codeSize;
                if (funcId >= funcs.size()) funcs.resize(funcId + 1);
                funcs[funcId] = fd;
                pc += codeSize;
            } else if (op == 0x03) { // HALT
                break;
            } else {
                skipInstruction(op);
            }
        }
        pc = savedPC;
    }

    void skipInstruction(uint8_t op) {
        switch (op) {
            case 0x01: pc += 2; break;
            case 0x02: pc += 2; break;
            case 0x04: pc += 2; break;
            case 0x05: pc += 2; break;
            case 0x30: pc += 2; break;
            case 0x31: pc += 2; break;
            case 0x41: pc += 3; break;
            case 0x2A: pc += 2; break;
            case 0x2B: pc += 2; break;
            default: break;
        }
    }

    void run() {
        preScanFunctions();
        pc = 0;
        while (pc < code.size()) {
            uint8_t op = code[pc++];
            switch (op) {
                // ─── 基础 ───
                case 0x00: break; // NOP

                case 0x01: { // LOAD_CONST u16(idx)
                    uint16_t idx = readU16(code, pc);
                    const Const& c = constPool[idx];
                    if (c.tag == Const::UTF8) {
                        push(Value(make_shared<string>(c.s)));
                    } else if (c.tag == Const::INT32) {
                        push(Value((int64_t)c.i));
                    } else if (c.tag == Const::DOUBLE) {
                        push(Value(c.d));
                    }
                    break;
                }

                case 0x02: { // CALL u8(funcId) u8(argc)  —— 内置函数
                    uint8_t funcId = readU8(code, pc);
                    uint8_t argc   = readU8(code, pc);
                    invoke(funcId, argc);
                    break;
                }

                case 0x03: return; // HALT

                // ─── 全局变量 ───
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

                // ─── 算术 ───
                case 0x06: { Value r = pop(), l = pop(); push(Value(l.i + r.i)); break; }
                case 0x07: { Value r = pop(), l = pop(); push(Value(l.i - r.i)); break; }
                case 0x08: { Value r = pop(), l = pop(); push(Value(l.i * r.i)); break; }
                case 0x09: { // IDIV
                    Value r = pop(), l = pop();
                    if (r.i == 0) throw runtime_error("整数除以零");
                    push(Value(l.i / r.i));
                    break;
                }
                case 0x0A: { // FADD（浮点加法，也用于字符串拼接的运行时）
                    Value r = pop(), l = pop();
                    // 如果两侧任一为字符串 → 字符串拼接
                    if (l.kind == VAL_STR || r.kind == VAL_STR) {
                        push(Value(toString(l) + toString(r)));
                    } else {
                        push(Value(toDouble(l) + toDouble(r)));
                    }
                    break;
                }
                case 0x0B: { Value r = pop(), l = pop(); push(Value(toDouble(l) - toDouble(r))); break; }
                case 0x0C: { Value r = pop(), l = pop(); push(Value(toDouble(l) * toDouble(r))); break; }
                case 0x0D: { // FDIV
                    Value r = pop(), l = pop();
                    double rd = toDouble(r);
                    if (rd == 0.0) throw runtime_error("浮点除以零");
                    push(Value(toDouble(l) / rd));
                    break;
                }
                case 0x0E: { Value v = pop(); push(Value((double)toDouble(v))); break; }
                case 0x0F: { // IMOD
                    Value r = pop(), l = pop();
                    if (r.i == 0) throw runtime_error("取模除以零");
                    push(Value((int64_t)(l.i % r.i)));
                    break;
                }

                // ─── 整数比较（字符串按字典序派发）───
                case 0x10: { Value r = pop(), l = pop();
                    if (l.kind == VAL_STR && r.kind == VAL_STR) push(Value((bool)(*l.str == *r.str)));
                    else { push(Value((bool)(l.i == r.i))); }
                    break; }
                case 0x11: { Value r = pop(), l = pop();
                    if (l.kind == VAL_STR && r.kind == VAL_STR) push(Value((bool)(*l.str != *r.str)));
                    else { push(Value((bool)(l.i != r.i))); }
                    break; }
                case 0x12: { Value r = pop(), l = pop();
                    if (l.kind == VAL_STR && r.kind == VAL_STR) push(Value((bool)(*l.str <  *r.str)));
                    else { push(Value((bool)(l.i <  r.i))); }
                    break; }
                case 0x13: { Value r = pop(), l = pop();
                    if (l.kind == VAL_STR && r.kind == VAL_STR) push(Value((bool)(*l.str <= *r.str)));
                    else { push(Value((bool)(l.i <= r.i))); }
                    break; }
                case 0x14: { Value r = pop(), l = pop();
                    if (l.kind == VAL_STR && r.kind == VAL_STR) push(Value((bool)(*l.str >  *r.str)));
                    else { push(Value((bool)(l.i >  r.i))); }
                    break; }
                case 0x15: { Value r = pop(), l = pop();
                    if (l.kind == VAL_STR && r.kind == VAL_STR) push(Value((bool)(*l.str >= *r.str)));
                    else { push(Value((bool)(l.i >= r.i))); }
                    break; }

                // ─── 浮点比较 ───
                case 0x16: { Value r = pop(), l = pop(); push(Value((bool)(toDouble(l) == toDouble(r)))); break; }
                case 0x17: { Value r = pop(), l = pop(); push(Value((bool)(toDouble(l) != toDouble(r)))); break; }
                case 0x18: { Value r = pop(), l = pop(); push(Value((bool)(toDouble(l) <  toDouble(r)))); break; }
                case 0x19: { Value r = pop(), l = pop(); push(Value((bool)(toDouble(l) <= toDouble(r)))); break; }
                case 0x1A: { Value r = pop(), l = pop(); push(Value((bool)(toDouble(l) >  toDouble(r)))); break; }
                case 0x1B: { Value r = pop(), l = pop(); push(Value((bool)(toDouble(l) >= toDouble(r)))); break; }

                // ─── 控制流 ───
                case 0x2A: { // JZ
                    uint16_t u = readU16(code, pc);
                    Value v = pop();
                    if (isFalse(v)) pc = (size_t)((int64_t)pc + (int16_t)u);
                    break;
                }
                case 0x2B: { // JMP
                    uint16_t u = readU16(code, pc);
                    pc = (size_t)((int64_t)pc + (int16_t)u);
                    break;
                }

                // ─── 局部变量 ───
                case 0x30: { // LOCAL_STORE u16(slot)
                    uint16_t slot = readU16(code, pc);
                    if (!currentLocals) throw runtime_error("LOCAL_STORE 在函数外无效");
                    if (slot >= currentLocals->size()) currentLocals->resize(slot + 1);
                    (*currentLocals)[slot] = pop();
                    break;
                }
                case 0x31: { // LOCAL_LOAD u16(slot)
                    uint16_t slot = readU16(code, pc);
                    if (!currentLocals) throw runtime_error("LOCAL_LOAD 在函数外无效");
                    if (slot >= currentLocals->size()) currentLocals->resize(slot + 1);
                    push((*currentLocals)[slot]);
                    break;
                }

                // ─── 函数定义 ───
                case 0x40: { // FUNC_DEF
                    uint16_t funcId    = readU16(code, pc);
                    uint16_t paramCount = readU16(code, pc);
                    uint16_t localCount = readU16(code, pc);
                    uint32_t codeSize  = readU32(code, pc);
                    FuncDef fd;
                    fd.funcId     = funcId;
                    fd.paramCount = paramCount;
                    fd.localCount = localCount;
                    fd.entryPC    = (uint32_t)pc;
                    fd.codeSize   = codeSize;
                    if (funcId >= funcs.size()) funcs.resize(funcId + 1);
                    funcs[funcId] = fd;
                    pc += codeSize;
                    break;
                }

                // ─── 函数调用 / 返回 ───
                case 0x41: { // CALL_FUNC u16(funcId) u8(argc)
                    uint16_t funcId = readU16(code, pc);
                    uint8_t  argc   = readU8(code, pc);
                    callUserFunction(funcId, argc);
                    break;
                }

                case 0x42: { // RET
                    if (callStack.empty()) {
                        return;
                    }
                    CallFrame& frame = callStack.back();
                    size_t retPC = frame.returnPC;
                    callStack.pop_back();
                    if (callStack.empty()) {
                        currentLocals = nullptr;
                    } else {
                        currentLocals = &(callStack.back().locals);
                    }
                    pc = retPC;
                    break;
                }

                case 0x43: { // POP
                    popValue();
                    break;
                }

                default:
                    throw runtime_error("未知指令: 0x" + to_string(op));
            }
        }
    }

    // 调用用户自定义函数（支持递归）
    void callUserFunction(uint16_t funcId, uint8_t argc) {
        if (funcId >= funcs.size()) throw runtime_error("调用未定义的函数 id=" + to_string(funcId));
        const FuncDef& fd = funcs[funcId];

        vector<Value> args(argc);
        for (int i = argc - 1; i >= 0; i--) {
            args[i] = pop();
        }

        CallFrame frame;
        frame.funcId   = funcId;
        frame.returnPC = pc;
        frame.locals.resize(fd.localCount);
        for (uint16_t i = 0; i < argc && i < fd.paramCount; i++) {
            frame.locals[i] = args[i];
        }

        callStack.push_back(move(frame));
        currentLocals = &(callStack.back().locals);

        pc = fd.entryPC;
    }

    // 调用内置函数（funcId 0/1/2/3/4/5/6）
    //   0=pr, 1=inp, 2=prln, 3=len, 4=str, 5=int, 6=float
    void invoke(uint8_t funcId, uint8_t argc) {
        if (funcId == 0 || funcId == 2) {
            // pr(...) / prln(...)
            vector<Value> args(argc);
            for (uint8_t i = 0; i < argc; i++) args[argc - 1 - i] = pop();

            for (uint8_t i = 0; i < argc; i++) {
                if (i > 0) cout << " ";
                printValue(args[i]);
            }
            if (funcId == 2) cout << endl;
        } else if (funcId == 1) {
            // inp()
            if (argc > 0) pop(); // 忽略多余参数
            string line;
            if (!getline(cin, line)) {
                throw runtime_error("inp() 读取输入失败（遇到 EOF）");
            }
            push(Value(make_shared<string>(line)));
        } else if (funcId == 3) {
            // len(x) → int
            Value v = pop();
            int64_t len = 0;
            if (v.kind == VAL_STR) len = v.str ? (int64_t)v.str->size() : 0;
            else if (v.kind == VAL_BOOL) len = 0;
            else if (v.kind == VAL_INT) {
                // 整数的十进制位数
                int64_t n = v.i;
                if (n < 0) n = -n;
                if (n == 0) len = 1;
                else { while (n > 0) { len++; n /= 10; } }
            } else len = 0;
            push(Value(len));
        } else if (funcId == 4) {
            // str(x) → string
            Value v = pop();
            push(Value(make_shared<string>(toString(v))));
        } else if (funcId == 5) {
            // int(x) → int64
            Value v = pop();
            push(Value(toInt(v)));
        } else if (funcId == 6) {
            // float(x) → double
            Value v = pop();
            push(Value(toDouble(v)));
        } else {
            throw runtime_error("调用未定义的函数 id=" + to_string(funcId));
        }
    }

    void popValue() { (void)pop(); }

    void printValue(const Value& v) {
        if (v.kind == VAL_STR) {
            if (v.str) cout << *(v.str);
        } else if (v.kind == VAL_BOOL) {
            cout << (v.i ? "true" : "false");
        } else if (v.kind == VAL_DOUBLE) {
            if (v.f == (double)(int64_t)v.f) cout << (int64_t)v.f;
            else cout << v.f;
        } else {
            cout << v.i;
        }
    }

    void load(const string& path) {
        ifstream f(path, ios::binary);
        if (!f) throw runtime_error("无法打开: " + path);
        vector<uint8_t> all((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
        if (all.size() < 16) throw runtime_error("文件过短，不是有效的 .aeo 文件");

        size_t p = 0;
        if (all[p]!='A'||all[p+1]!='e'||all[p+2]!='B'||all[p+3]!='c')
            throw runtime_error("魔数不匹配");
        p += 4;
        p += 2; // major, minor
        p += 2; // flags

        uint32_t poolCount = readU32(all, p);
        if (poolCount > 1024u * 1024u) throw runtime_error("常量池过大: " + to_string(poolCount));
        constPool.resize(poolCount);
        for (uint32_t i = 0; i < poolCount; i++) {
            if (p >= all.size()) throw runtime_error("常量池截断（条目 #" + to_string(i) + "）");
            uint8_t tag = readU8(all, p);
            if (tag != Const::UTF8 && tag != Const::INT32 && tag != Const::DOUBLE)
                throw runtime_error("未知的常量池 tag: 0x" + to_string(tag) + "（条目 #" + to_string(i) + "）");
            constPool[i].tag = (decltype(Const::tag))tag;
            if (tag == Const::UTF8) {
                if (p + 4 > all.size()) throw runtime_error("常量池 UTF-8 长度截断");
                uint32_t len = readU32(all, p);
                if (len > all.size() - p) throw runtime_error("常量池 UTF-8 数据截断（len=" + to_string(len) + "）");
                constPool[i].s = string((const char*)(all.data() + p), len);
                p += len;
            } else if (tag == Const::INT32) {
                if (p + 4 > all.size()) throw runtime_error("常量池 int32 截断");
                constPool[i].i = (int32_t)readU32(all, p);
            } else if (tag == Const::DOUBLE) {
                if (p + 8 > all.size()) throw runtime_error("常量池 double 截断");
                uint64_t bits = 0;
                for (int s = 56; s >= 0; s -= 8) bits = (bits << 8) | readU8(all, p);
                memcpy(&constPool[i].d, &bits, 8);
            }
        }

        if (p + 4 > all.size()) throw runtime_error("codeLength 字段截断");
        uint32_t codeLen = readU32(all, p);
        if (codeLen > all.size() - p) throw runtime_error("codeLength 越界: " + to_string(codeLen) + " > 剩余 " + to_string(all.size() - p) + " 字节");
        if (codeLen > 16u * 1024u * 1024u) throw runtime_error("codeLength 过大: " + to_string(codeLen));
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