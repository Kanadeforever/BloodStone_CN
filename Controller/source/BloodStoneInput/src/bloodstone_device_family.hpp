#pragma once
#include <cstdint>
#include <cstring>

namespace bondone {
constexpr char LowerAscii(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}
inline bool ContainsAscii(const char* source, const char* token) noexcept {
    if (!source || !token || !*token) return false;
    for (const char* start = source; *start; ++start) {
        const char* a = start;
        const char* b = token;
        while (*a && *b && LowerAscii(*a) == LowerAscii(*b)) { ++a; ++b; }
        if (!*b) return true;
    }
    return false;
}
// SDL 的有线/蓝牙 GUID 和 PID 可以不同；只在经过游戏手柄类别校验后用于原生输入去重。
inline bool IsStadia(std::uint16_t vendor, const char* name) noexcept {
    return ContainsAscii(name, "stadia") || vendor == 0x18D1;
}
}
