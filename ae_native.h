// =============================================================================
//  Ae 原生库 ABI  ——  ae_native.h
// -----------------------------------------------------------------------------
//  写原生库只需要 include 这一个头文件。**不要** include VM 的内部头
//  （Value / Table 的布局随时会变，而 ABI 承诺稳定）。
//
//  设计要点
//    ① 宿主（aec / ae）加载 <name>.dll 并取导出符号 `ae_module`。
//    ② 宿主用 setApi() 把函数表【注入】给库 —— 库因此不依赖宿主的任何导出符号，
//       直接 `clang++ -shared io.cpp -o io.dll` 就能编译，不需要 import lib。
//    ③ 原生函数签名： int fn(AeVM* vm, int argc, AeValue* argv)
//         · 返回值 = 压入 VM 栈的值个数（0 或 1 常见，也支持多返回值 / 返回 0 个）
//         · 参数通过 argv 只读访问；要返回就在函数里 push
//    ④ 两类错误分开：
//         · 预期失败（文件不存在、目录为空…）→ 返回 null/false + ae_set_error("原因")
//           ，脚本侧用 <lib>.lastError() 取原因，程序继续跑
//         · 程序性错误（参数类型不对、句柄非法…）→ ae_raise(vm, "...")
//           ，立刻变成带调用栈的运行错误
//
//  最小骨架：
//      #include "ae_native.h"
//      static int nat_hello(AeVM* vm, int argc, AeValue* argv) {
//          ae_push_str(vm, "hello", 5);
//          return 1;
//      }
//      static const AeNativeDef kExports[] = {
//          { "hello", 0, 0, nat_hello },
//      };
//      static const AeNativeDef* getExports(int* n) { *n = 1; return kExports; }
//      AE_MODULE("hello", getExports)
// =============================================================================
#ifndef AE_NATIVE_H
#define AE_NATIVE_H

#include <stdint.h>
#include <stddef.h>

#define AE_ABI_VERSION 4       // v2：脚本参数/路径；v3：读表；v4：回调 Ae 函数（func_retain/call）

// 库导出符号的可见性（Windows 上 -shared 不会自动导出，必须显式 dllexport）
#ifdef _WIN32
  #define AE_EXPORT __declspec(dllexport)
#else
  #define AE_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeVM AeVM;

// 与 VM 内部 ValueKind 一一对应（不要改数值）
enum {
    AE_NULL   = 0,
    AE_INT    = 1,
    AE_FLOAT  = 2,
    AE_STRING = 3,
    AE_BOOL   = 4,
    AE_TABLE  = 5,
    AE_FUNC   = 6    // ★ ABI v4：闭包（函数值）
};

// 不透明值：只用下面的 API 操作，别直接读 p
typedef struct AeValue {
    int      kind;
    int64_t  i;
    double   f;
    const void* p;      // AE_STRING → std::string*；AE_TABLE → Table*
} AeValue;

typedef int (*AeNativeFn)(AeVM* vm, int argc, AeValue* argv);

typedef struct AeNativeDef {
    const char* name;
    int         minArgs;   // -1 表示不限
    int         maxArgs;   // -1 表示不限
    AeNativeFn  fn;
} AeNativeDef;

// 宿主注入的函数表
typedef struct AeApi {
    int abiVersion;
    /* 压栈 */
    void (*push_null) (AeVM*);
    void (*push_int)  (AeVM*, int64_t);
    void (*push_float)(AeVM*, double);
    void (*push_bool) (AeVM*, int);
    void (*push_str)  (AeVM*, const char* s, size_t len);
    void (*push_table)(AeVM*, AeValue t);
    /* 构造 */
    AeValue (*make_str)(AeVM*, const char* s, size_t len);
    AeValue (*new_table)(AeVM*);
    void    (*table_set)(AeVM*, AeValue t, AeValue k, AeValue v);
    /* 错误 */
    void (*set_error)(const char* msg);   // 预期失败：记录原因，不中断
    void (*raise)    (AeVM*, const char* msg);  // 程序性错误：变成运行错误
    /* ★ ABI v2：宿主环境查询（脚本参数 / 脚本路径） */
    int         (*arg_count)(AeVM*);
    const char* (*arg_get)  (AeVM*, int idx, size_t* len);
    const char* (*script_path)(AeVM*, size_t* len);
    /* ★ ABI v3：读表 —— 原生库要能处理 {x = 1} 这种选项表 / {x1,y1,x2,y2} 这种点数组
       · table_get 返回 1 表示取到了，0 表示键不存在（out 置 null）
       · 取到字符串时 out->p 指向表里那份 std::string，【借用】即可，
         只要表还活着就有效（原生调用期间一定活着），不要 free
       · table_len = 数组段长度（连续整数键前缀），不是"非空项个数" */
    int         (*table_get)(AeVM*, AeValue t, AeValue k, AeValue* out);
    int         (*table_len)(AeVM*, AeValue t);
    /* ★ ABI v4：回调 Ae 函数（ui 的事件 handler 靠这套）
       · 闭包用【句柄】持有：func_retain 拿到 >0 的 handle，不用了必须 func_release
         （为什么不直接存 AeValue：那是个借用指针，脚本一挪栈就悬空了）
       · func_call 的参数用你自己造的值（整数/字符串/表都行；表用 new_table 造）
       · 成功返回 1 并把结果写进 *out；失败返回 0，原因写进 *err
         失败【不会】自动中断脚本：库要么用 raise() 转成运行错误，要么自己忽略。
         回调内部发生的一切（包括再嵌套调用原生库）都是安全的。 */
    int         (*func_retain)(AeVM*, AeValue fn);
    void        (*func_release)(AeVM*, int handle);
    int         (*func_call)(AeVM*, int handle, int argc, const AeValue* argv,
                             AeValue* out, const char** err);
    /* 扩展位（宿主内部用，库不要碰） */
    void* reserved[1];
} AeApi;

typedef struct AeModule {
    int abiVersion;
    const char* moduleName;
    const AeNativeDef* (*getExports)(int* count);
    void (*setApi)(const AeApi* api);
} AeModule;

#ifdef __cplusplus
}  // extern "C"
#endif

// =============================================================================
//  C++ 便捷包装：库代码直接调 ae_push_str(...) 就行
// =============================================================================
#ifdef __cplusplus
#include <string>
namespace ae_abi {
    inline const AeApi* g_api = nullptr;      // C++17 inline 变量：每个模块一份
    inline int         g_abiVersion = 0;
}

inline int64_t ae_to_int(AeValue v)               { return v.i; }
inline double  ae_to_double(AeValue v)            { return v.f; }
inline const char* ae_to_str(AeValue v, size_t* n) {
    const std::string* s = (const std::string*)v.p;
    if (!s) { if (n) *n = 0; return ""; }
    if (n) *n = s->size();
    return s->data();
}
inline int ae_is_null (AeValue v) { return v.kind == AE_NULL; }
inline int ae_is_int  (AeValue v) { return v.kind == AE_INT; }
inline int ae_is_float(AeValue v) { return v.kind == AE_FLOAT; }
inline int ae_is_num  (AeValue v) { return v.kind == AE_INT || v.kind == AE_FLOAT; }
inline int ae_is_str  (AeValue v) { return v.kind == AE_STRING; }
inline int ae_is_bool (AeValue v) { return v.kind == AE_BOOL; }
inline int ae_is_table(AeValue v) { return v.kind == AE_TABLE; }
inline int ae_is_func (AeValue v) { return v.kind == AE_FUNC;  }

inline void ae_push_null (AeVM* vm)                    { ae_abi::g_api->push_null(vm); }
inline void ae_push_int  (AeVM* vm, int64_t x)         { ae_abi::g_api->push_int(vm, x); }
inline void ae_push_float(AeVM* vm, double x)          { ae_abi::g_api->push_float(vm, x); }
inline void ae_push_bool (AeVM* vm, int b)             { ae_abi::g_api->push_bool(vm, b); }
inline void ae_push_str  (AeVM* vm, const std::string& s) { ae_abi::g_api->push_str(vm, s.data(), s.size()); }
inline void ae_push_str  (AeVM* vm, const char* s)     { ae_abi::g_api->push_str(vm, s, s ? std::char_traits<char>::length(s) : 0); }
inline void ae_push_table(AeVM* vm, AeValue t)         { ae_abi::g_api->push_table(vm, t); }
inline AeValue ae_make_str(AeVM* vm, const std::string& s) { return ae_abi::g_api->make_str(vm, s.data(), s.size()); }
inline AeValue ae_make_str(AeVM* vm, const char* s)    { return ae_abi::g_api->make_str(vm, s, s ? std::char_traits<char>::length(s) : 0); }
inline AeValue ae_new_table(AeVM* vm)                  { return ae_abi::g_api->new_table(vm); }
inline void ae_table_set(AeVM* vm, AeValue t, AeValue k, AeValue v) { ae_abi::g_api->table_set(vm, t, k, v); }
inline void ae_set_error(const std::string& m)         { ae_abi::g_api->set_error(m.c_str()); }
inline void ae_raise(AeVM* vm, const std::string& m)   { ae_abi::g_api->raise(vm, m.c_str()); }

// ★ ABI v2：脚本的命令行参数与脚本路径
inline int ae_arg_count(AeVM* vm) { return ae_abi::g_api->arg_count(vm); }
inline std::string ae_arg(AeVM* vm, int idx) {
    size_t n = 0;
    const char* p = ae_abi::g_api->arg_get(vm, idx, &n);
    return p ? std::string(p, n) : std::string();
}
inline std::string ae_script_path(AeVM* vm) {
    size_t n = 0;
    const char* p = ae_abi::g_api->script_path(vm, &n);
    return p ? std::string(p, n) : std::string();
}

// ★ ABI v4：回调 Ae 函数（句柄式，见 AeApi 里 func_retain 的说明）
inline int  ae_retain_func (AeVM* vm, AeValue fn) { return ae_abi::g_api->func_retain(vm, fn); }
inline void ae_release_func(AeVM* vm, int h)      { if (h > 0) ae_abi::g_api->func_release(vm, h); }
inline bool ae_call_func(AeVM* vm, int h, int argc, const AeValue* argv,
                         AeValue* out, std::string* err) {
    const char* e = nullptr;
    int ok = ae_abi::g_api->func_call(vm, h, argc, argv, out, &e);
    if (!ok && err) *err = e ? e : "回调失败";
    return ok != 0;
}
inline bool ae_call_func1(AeVM* vm, int h, AeValue arg, AeValue* out, std::string* err) {
    return ae_call_func(vm, h, 1, &arg, out, err);
}

// ★ ABI v3：读脚本传进来的表（选项表 / 点数组）
inline bool ae_table_get(AeVM* vm, AeValue t, AeValue k, AeValue* out) {
    if (!ae_abi::g_api->table_get) return false;
    return ae_abi::g_api->table_get(vm, t, k, out) != 0;
}
inline bool ae_table_get(AeVM* vm, AeValue t, const char* key, AeValue* out) {
    return ae_table_get(vm, t, ae_make_str(vm, key), out);
}
inline int ae_table_len(AeVM* vm, AeValue t) {
    if (!ae_abi::g_api->table_len) return 0;
    return ae_abi::g_api->table_len(vm, t);
}
// 取值 + 类型判断的小糖（取不到 / 类型不对 → false）
inline bool ae_table_num(AeVM* vm, AeValue t, const char* key, double* out) {
    AeValue v;
    if (!ae_table_get(vm, t, key, &v)) return false;
    if (v.kind == AE_INT)   { if (out) *out = (double)v.i; return true; }
    if (v.kind == AE_FLOAT) { if (out) *out = v.f;         return true; }
    if (v.kind == AE_BOOL)  { if (out) *out = (double)(v.i ? 1 : 0); return true; }
    return false;
}
inline bool ae_table_bool(AeVM* vm, AeValue t, const char* key, bool* out) {
    AeValue v;
    if (!ae_table_get(vm, t, key, &v)) return false;
    if (v.kind == AE_BOOL) { if (out) *out = (v.i != 0); return true; }
    if (v.kind == AE_INT)  { if (out) *out = (v.i != 0); return true; }
    return false;
}
inline bool ae_table_str(AeVM* vm, AeValue t, const char* key, std::string* out) {
    AeValue v;
    if (!ae_table_get(vm, t, key, &v)) return false;
    if (v.kind != AE_STRING) return false;
    size_t n = 0;
    const char* p = ae_to_str(v, &n);
    if (out) out->assign(p, n);
    return true;
}

// 常用：往表里塞字符串 / 取字符串参数（越界时 ae_raise）
inline AeValue ae_str_value(AeVM* vm, const std::string& s) { return ae_make_str(vm, s); }
inline AeValue ae_int_value(int64_t x) { AeValue v; v.kind = AE_INT; v.i = x; v.f = 0; v.p = nullptr; return v; }
inline AeValue ae_bool_value(int b)    { AeValue v; v.kind = AE_BOOL; v.i = b ? 1 : 0; v.f = 0; v.p = nullptr; return v; }

#endif  // __cplusplus

// 模块导出宏：放在文件末尾（整个库只能出现一次）
#define AE_MODULE(modname, exportsfn)                                        \
    extern "C" {                                                             \
    static void ae_module_set_api_impl(const AeApi* api) {                    \
        ae_abi::g_api = api;                                                 \
        ae_abi::g_abiVersion = api ? api->abiVersion : 0;                    \
    }                                                                        \
    AE_EXPORT extern const AeModule ae_module = {                            \
        AE_ABI_VERSION, modname, exportsfn, ae_module_set_api_impl           \
    };                                                                       \
    }

#endif  // AE_NATIVE_H
