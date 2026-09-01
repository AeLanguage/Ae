// =============================================================================
//  Ae 虚拟机 (ae = Ae VM)  ——  支持自定义函数 + 局部变量 + 递归
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
        return 0.0;
    }

    bool isFalse(const Value& v) const {
        if (v.kind == VAL_BOOL)   return v.i == 0;
        if (v.kind == VAL_INT)    return v.i == 0;
        if (v.kind == VAL_DOUBLE) return v.f == 0.0;
        if (v.kind == VAL_STR)    return v.str == nullptr || *v.str == "";
        return false;
    }

    // 预处理：扫描所有 FUNC_DEF 指令，建立函数表
    // 这样 CALL_FUNC 时函数已注册（即使定义在使用之后）
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
                fd.entryPC    = (uint32_t)pc;  // 紧接着的就是函数体
                fd.codeSize   = codeSize;
                if (funcId >= funcs.size()) funcs.resize(funcId + 1);
                funcs[funcId] = fd;
                pc += codeSize; // 跳过函数体
            } else if (op == 0x03) { // HALT
                break; // 不应到达函数定义区（在 HALT 之后）
            } else {
                // 其他指令：粗略跳过（对于预处理够用）
                skipInstruction(op);
            }
        }
        pc = savedPC; // 恢复到 0
    }

    // 粗略跳过一条指令的操作数（预处理用）
    void skipInstruction(uint8_t op) {
        switch (op) {
            case 0x01: pc += 2; break; // LOAD_CONST u16
            case 0x02: pc += 2; break; // CALL u8 u8
            case 0x04: pc += 2; break; // STORE u16
            case 0x05: pc += 2; break; // LOAD_VAR u16
            case 0x30: pc += 2; break; // LOCAL_STORE u16
            case 0x31: pc += 2; break; // LOCAL_LOAD u16
            case 0x41: pc += 3; break; // CALL_FUNC u16 u8
            case 0x2A: pc += 2; break; // JZ u16
            case 0x2B: pc += 2; break; // JMP u16
            // 其余为无操作数指令，pc 已在调用处前进 1
            default: break;
        }
    }

    void run() {
        preScanFunctions(); // 先建立函数表
        pc = 0;            // 从头开始执行
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
                case 0x0A: { Value r = pop(), l = pop(); push(Value(toDouble(l) + toDouble(r))); break; }
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

                // ─── 整数比较 ───
                case 0x10: { Value r = pop(), l = pop(); push(Value((bool)(l.i == r.i))); break; }
                case 0x11: { Value r = pop(), l = pop(); push(Value((bool)(l.i != r.i))); break; }
                case 0x12: { Value r = pop(), l = pop(); push(Value((bool)(l.i <  r.i))); break; }
                case 0x13: { Value r = pop(), l = pop(); push(Value((bool)(l.i <= r.i))); break; }
                case 0x14: { Value r = pop(), l = pop(); push(Value((bool)(l.i >  r.i))); break; }
                case 0x15: { Value r = pop(), l = pop(); push(Value((bool)(l.i >= r.i))); break; }

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
                case 0x40: { // FUNC_DEF u16(funcId) u16(paramCount) u16(localCount) u32(codeSize)
                    uint16_t funcId    = readU16(code, pc);
                    uint16_t paramCount = readU16(code, pc);
                    uint16_t localCount = readU16(code, pc);
                    uint32_t codeSize  = readU32(code, pc);
                    FuncDef fd;
                    fd.funcId     = funcId;
                    fd.paramCount = paramCount;
                    fd.localCount = localCount;
                    fd.entryPC    = (uint32_t)pc;  // 紧接着的就是函数体
                    fd.codeSize   = codeSize;
                    // 确保函数表足够大
                    if (funcId >= funcs.size()) funcs.resize(funcId + 1);
                    funcs[funcId] = fd;
                    // 跳过函数体（在调用时才执行）
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
                    // 弹出当前帧，恢复返回地址
                    if (callStack.empty()) {
                        // 顶层 return → 等同于 HALT
                        return;
                    }
                    CallFrame& frame = callStack.back();
                    size_t retPC = frame.returnPC;
                    callStack.pop_back();
                    // 恢复 currentLocals
                    if (callStack.empty()) {
                        currentLocals = nullptr;
                    } else {
                        currentLocals = &(callStack.back().locals);
                    }
                    pc = retPC;
                    break;
                }

                case 0x43: { // POP  丢弃栈顶（语句级调用丢弃返回值）
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

        // 收集参数（从栈上弹出，顺序是 param0..paramN-1）
        vector<Value> args(argc);
        for (int i = argc - 1; i >= 0; i--) {
            args[i] = pop();
        }

        // 创建新帧
        CallFrame frame;
        frame.funcId   = funcId;
        frame.returnPC = pc;  // 记录返回地址（CALL_FUNC 指令后的位置）
        // 局部变量区：参数在前，然后是普通局部变量
        frame.locals.resize(fd.localCount);
        for (uint16_t i = 0; i < argc && i < fd.paramCount; i++) {
            frame.locals[i] = args[i];
        }
        // 剩余参数位置保持默认零值

        // 压入调用栈
        callStack.push_back(move(frame));
        currentLocals = &(callStack.back().locals);

        // 跳转到函数入口
        pc = fd.entryPC;
        // 注意：不在这里执行函数体，run() 循环会继续从 pc 处执行
        // 函数体执行完毕后遇到 RET 指令返回
    }

    // 调用内置函数（funcId 0/1/2）
    // 约定：调用前栈顶向下的 argc 个值就是本次实参。我们只消费这 argc
    // 个参数，绝不触碰栈中更早的残留值（例如未被接收的函数返回值）。
    void invoke(uint8_t funcId, uint8_t argc) {
        if (funcId == 0 || funcId == 2) {
            // pr(...) / prln(...)
            // 从栈上弹出正好 argc 个实参（参数按从左到右的顺序入栈，故先弹出的是最右）
            vector<Value> args(argc);
            for (uint8_t i = 0; i < argc; i++) args[argc - 1 - i] = pop();

            for (uint8_t i = 0; i < argc; i++) {
                if (i > 0) cout << " ";
                printValue(args[i]);
            }
            if (funcId == 2) cout << endl;  // prln 附加换行
        } else if (funcId == 1) {
            // inp(...)
            string line;
            if (!getline(cin, line)) {
                throw runtime_error("inp() 读取输入失败（遇到 EOF）");
            }
            push(Value(make_shared<string>(line)));
        } else {
            throw runtime_error("调用未定义的函数 id=" + to_string(funcId));
        }
    }

    // 丢弃栈顶值（用于"语句级函数调用"丢弃返回值，防止栈增长 / 误打印）
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
        constPool.resize(poolCount);
        for (uint32_t i = 0; i < poolCount; i++) {
            uint8_t tag = readU8(all, p);
            constPool[i].tag = (decltype(Const::tag))tag;
            if (tag == Const::UTF8) {
                uint32_t len = readU32(all, p);
                constPool[i].s = string((const char*)(all.data() + p), len);
                p += len;
            } else if (tag == Const::INT32) {
                constPool[i].i = (int32_t)readU32(all, p);
            } else if (tag == Const::DOUBLE) {
                uint64_t bits = 0;
                for (int s = 56; s >= 0; s -= 8) bits = (bits << 8) | readU8(all, p);
                memcpy(&constPool[i].d, &bits, 8);
            }
        }

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