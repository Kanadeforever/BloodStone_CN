#pragma once

// 只检查 Windows 命令行中的独立参数 --log，不改变游戏自己的参数处理。
// 游戏可执行文件名称是第一个参数，必须跳过，避免安装路径中带有 --log 时误开启日志。
// 双引号和反斜杠按 Windows C 运行时的常见参数规则解释，确保带空格的路径不影响识别。
// 这里不分配内存，也不依赖额外的 C 运行时，适合在 DLL 初始化时调用。
namespace bscn {

inline bool CommandSpace(wchar_t ch) {
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

inline bool HasLogArgument(const wchar_t* input) {
    if (!input) return false;
    const wchar_t* cursor = input;
    unsigned long argument = 0;
    while (*cursor) {
        while (CommandSpace(*cursor)) ++cursor;
        if (!*cursor) break;
        bool quoted = false;
        unsigned long chars = 0;
        bool matches = true;
        constexpr wchar_t wanted[] = L"--log";
        while (*cursor && (quoted || !CommandSpace(*cursor))) {
            unsigned long slashes = 0;
            while (*cursor == L'\\') { ++slashes; ++cursor; }
            if (*cursor == L'"') {
                // 反斜杠成对地表示普通反斜杠；奇数个则把引号当作字面字符。
                for (unsigned long i = 0; i < slashes / 2; ++i) {
                    if (chars >= 5 || wanted[chars] != L'\\') matches = false;
                    ++chars;
                }
                if (slashes & 1) {
                    if (chars >= 5 || wanted[chars] != L'"') matches = false;
                    ++chars;
                    ++cursor;
                } else if (quoted && cursor[1] == L'"') {
                    if (chars >= 5 || wanted[chars] != L'"') matches = false;
                    ++chars;
                    cursor += 2;
                } else {
                    quoted = !quoted;
                    ++cursor;
                }
                continue;
            }
            for (unsigned long i = 0; i < slashes; ++i) {
                if (chars >= 5 || wanted[chars] != L'\\') matches = false;
                ++chars;
            }
            if (!*cursor || (!quoted && CommandSpace(*cursor))) break;
            if (chars >= 5 || wanted[chars] != *cursor) matches = false;
            ++chars;
            ++cursor;
        }
        if (argument > 0 && matches && chars == 5) return true;
        ++argument;
    }
    return false;
}

}  // namespace bscn
