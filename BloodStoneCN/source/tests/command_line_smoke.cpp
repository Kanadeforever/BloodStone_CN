#include "../src/command_line.h"

// 这些测试只执行纯参数解析器，不需要运行游戏，也不调用 Windows API。
// 第一个命令行参数总是游戏可执行文件，即使文件名含 --log 也绝不能开启日志。
struct Case { const wchar_t* command; bool enabled; };
int main() {
    const Case examples[] = {
        {L"Bond.exe", false},
        {L"Bond.exe --log", true},
        {L"\"F:\\Games\\Bond.exe\" --log", true},
        {L"\"F:\\--log\\Bond.exe\"", false},
        {L"\"F:\\--log\\Bond.exe\" --other", false},
        {L"Bond.exe --logger", false},
        {L"Bond.exe --log=true", false},
        {L"Bond.exe --LOG", false},
        {L"Bond.exe --Log", false},
        {L"Bond.exe --other --log --more", true},
        {L"Bond.exe \"--log\"", true},
        {L"Bond.exe \"--log extra\"", false},
        {L"Bond.exe --logextra", false},
        {L"Bond.exe --no-log", false},
        {L"Bond.exe \"file with --log inside\"", false},
        {L"Bond.exe \t--log\r\n", true},
        {L"Bond.exe --log\\\"", false},
        {L"Bond.exe \\\"--log\\\"", false},
        {L"Bond.exe --log\\", false},
        {L"Bond.exe --lo", false},
        {L"Bond.exe --log --log", true},
        {nullptr, false},
    };
    for (unsigned long i = 0; i < sizeof(examples) / sizeof(examples[0]); ++i)
        if (bscn::HasLogArgument(examples[i].command) != examples[i].enabled)
            return static_cast<int>(i + 1);
    return 0;
}
