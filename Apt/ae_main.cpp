// ae 可执行文件 —— 薄壳
// 真正的命令行逻辑在 ae.cpp 的 aeRunMain() 里。之所以不直接叫 main：
// AOT 后端生成的 .cpp 会把 ae.cpp 整个 include 进去，由生成的代码提供
// 自己的 main；若 ae.cpp 里也有 main 就会重复定义。
// 构建： g++ ae.cpp ae_main.cpp -o ae
int aeRunMain(int argc, char** argv);

int main(int argc, char** argv) {
    return aeRunMain(argc, argv);
}
