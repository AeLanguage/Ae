// =============================================================================
//  ae_builtin.h —— 内置原生库登记表（静态链接，不再需要 .so / .dll）
// -----------------------------------------------------------------------------
//  改动背景：
//    原来的标准库（io / os / path / ui）都是「独立 .cpp → 编译成动态库 → 运行时
//    dlopen 加载」。这套机制要求用户先构建 4 个库文件，还要保证它们被放在 ae
//    找得到的目录里，是部署时最容易出问题的一环。
//
//  现在：
//    io / os / path 三个库改成【头文件版】（lib/ae_lib_*.h），由本文件 #include，
//    在编译 ae（虚拟机）与 aec（编译器）时直接链进可执行文件。于是：
//
//      · 构建产物只有两个可执行文件：aec 与 ae（不再有任何 .so / .dll）
//      · imp io / imp os / imp path 依旧照原样使用，脚本一行都不用改
//      · 用 Ae 自己写的 .m 模块（json / args）机制保持不变
//      · ui 库（Windows / GDI+）已整体移除，不在本项目范围内
//
//  扩展方式：
//    1) 新写一个 lib/ae_lib_xxx.h（照抄 io 的骨架：独立命名空间 + 末尾
//       getExports() / setApi()），在下面 include，并在 find() 里加一行；
//    2) 或者仍然可以放一个动态库到 lib/ 目录 —— 内置表里查不到的名字会回退到
//       原来的 aehost::loadLibrary 动态加载（ABI 版本校验照旧）。
//
//  关于 API 指针：三个库共用 ae_abi::g_api（ae_native.h 里的 inline 变量）。
//  宿主启动时对每个库注入的都是同一份 AeApi，所以共享是安全的。
// =============================================================================
#ifndef AE_BUILTIN_H
#define AE_BUILTIN_H

#include "ae_native.h"
#include <string>
#include <vector>

#include "lib/ae_lib_io.h"
#include "lib/ae_lib_os.h"
#include "lib/ae_lib_path.h"

namespace aebuiltin {

struct Module {
    const char* name;
    const AeNativeDef* (*getExports)(int*);   // 取导出表（函数指针 + 参数个数）
    void               (*setApi)(const AeApi*); // 注入宿主 API
};

// 查内置库；命中返回 true
inline bool find(const std::string& name, Module* out = nullptr) {
    if (name == "io") {
        if (out) *out = Module{"io",   &ae_lib_io::getExports,   &ae_lib_io::setApi};
        return true;
    }
    if (name == "os") {
        if (out) *out = Module{"os",   &ae_lib_os::getExports,   &ae_lib_os::setApi};
        return true;
    }
    if (name == "path") {
        if (out) *out = Module{"path", &ae_lib_path::getExports, &ae_lib_path::setApi};
        return true;
    }
    return false;
}

// 所有内置库的名字（用于帮助信息 / 报错提示）
inline std::vector<std::string> names() {
    return {"io", "os", "path"};
}

} // namespace aebuiltin

#endif // AE_BUILTIN_H
