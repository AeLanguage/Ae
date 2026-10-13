#pragma once
// 诊断渲染：把结构化消息变成专业报错
// 分类表集中在这里维护 —— 新增错误类型只改这一张表
#include <string>
#include <fstream>
#include <sstream>
#include "ae_diag.h"
#include "ae_common.h"

static const char* classifyDiag(const std::string& m) {
    struct Rule { const char* frag; const char* code; };
    static const Rule kRules[] = {
        { "语法错误: 期望",     AE_E_SYNTAX },
        { "语法错误: 无法解析", AE_E_SYNTAX },
        { "非法字符",           AE_E_BADCHAR },
        { "字符串字面量未闭合", AE_E_STR_UNCLOSED },
        { "块注释",             AE_E_CMT_UNCLOSED },
        { "未定义的函数",       AE_E_UNDEF_FUNC },
        { "未声明的变量",       AE_E_UNDECLARED },
        { "变量 '",             AE_E_UNDEF_VAR },
        { "重复定义",           AE_E_DUP_FUNC },
        { "是内建函数名",       AE_E_SHADOW_BUILTIN },
        { "参数数量不匹配",     AE_E_ARITY },
        { "至少需要",           AE_E_LIB_ARITY },
        { "最多接受",           AE_E_LIB_ARITY },
        { "里没有函数",         AE_E_LIB_NOFUNC },
        { "未导入的库",         AE_E_NO_IMPORT },
        { "找不到库",           AE_E_LIB_NOTFOUND },
        { "没有导出任何函数",   AE_E_LIB_NOTFOUND },
        { "ABI 版本",           AE_E_LIB_ABI },
        { "常量超出",           AE_E_NUM_RANGE },
        { "字面量缺少",         AE_E_NUM_RANGE },
        { "不是合法 UTF-8",     AE_E_BAD_UTF8 },
        { "break",              AE_E_LOOP_CTRL },
        { "continue",           AE_E_LOOP_CTRL },
        { "只能在函数内使用",   AE_E_SCOPE },
        { "多于左侧",           AE_E_MULTI_ASSIGN },
        { "表键",               AE_E_TABLE_LIT },
        { "main 不能有参数",    AE_E_MAIN_SIG },
    };
    for (const Rule& r : kRules)
        if (m.find(r.frag) != std::string::npos) return r.code;
    return AE_E_SYNTAX;
}
static std::string helpForDiag(const std::string& m) {
    struct Rule { const char* frag; const char* help; };
    static const Rule kRules[] = {
        { "未声明的变量", "赋值可以隐式创建全局变量；函数内请先用 local 声明。" },
        { "未定义的函数", "若这个函数来自原生库，请在文件里写 imp <库名>。" },
        { "未导入的库",   "库函数要先写 imp <库名>；表字段请写成 t[\"key\"]。" },
        { "找不到库",     "库文件应放在 <工具链目录>/lib/ 下，或用环境变量 AE_LIB_PATH 指定目录。" },
        { "参数数量不匹配", "用户函数的参数个数必须完全一致，没有默认值。" },
        { "不是合法 UTF-8", "请把源文件另存为 UTF-8（无 BOM 也可以）。" },
        { "多于左侧",     "多赋值时右侧表达式个数不能多于左侧目标个数。" },
    };
    for (const Rule& r : kRules)
        if (m.find(r.frag) != std::string::npos) return r.help;
    return std::string();
}
static std::string readWholeFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::string();
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
static void renderDiag(const std::string& what, const std::string& file,
                       const std::string& src, const std::vector<std::string>* files = nullptr) {
    int fi = 0, line = 0, col = 1, len = 1;
    std::string msg = what;
    if (aeUnpack(what, fi, line, col, len, msg)) {
        std::string useFile = file;
        std::string useSrc  = src;
        if (files && fi > 0 && (size_t)fi < files->size()) {
            useFile = (*files)[(size_t)fi];                 // 报错来自 .m 模块
            useSrc  = readWholeFile(useFile);
        }
        aediag::AeDiag d;
        d.code  = classifyDiag(msg);
        d.msg   = msg;
        d.span  = { useFile, line, col, len };
        d.hasSpan = true;
        std::string h = helpForDiag(msg);
        if (!h.empty()) d.helps.push_back(h);
        aediag::printDiag(d, useSrc);
    } else {
        // 兼容尚未改成结构化格式的抛出点：尽量从 "第 N 行 M 列: 消息" 里恢复位置
        std::string m2 = what;
        int l2 = 0, c2 = 1;
        if (what.rfind("\xe7\xac\xac ", 0) == 0) {               // 以 "第 " 开头
            size_t a = what.find(" \xe8\xa1\x8c ");               // " 行 "
            size_t b = what.find(" \xe5\x88\x97: ");              // " 列: "
            if (a != std::string::npos && b != std::string::npos) {
                l2 = atoi(what.substr(4, a - 4).c_str());
                c2 = atoi(what.substr(a + 5, b - a - 5).c_str());
                m2 = what.substr(b + 6);
            }
        }
        aediag::AeDiag d;
        d.code = classifyDiag(m2);
        d.msg  = m2;
        if (l2 > 0) { d.span = { file, l2, c2, 1 }; d.hasSpan = true; }
        std::string h = helpForDiag(m2);
        if (!h.empty()) d.helps.push_back(h);
        aediag::printDiag(d, src);
    }
}
