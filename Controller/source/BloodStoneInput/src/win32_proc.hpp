#pragma once

#include <windows.h>

#include <cstring>
#include <type_traits>

namespace joyshim {

template <typename T>
T FarProcToFunction(FARPROC address) noexcept {
    static_assert(std::is_pointer_v<T>, "T must be a function pointer");
    static_assert(sizeof(T) == sizeof(address), "Unexpected function pointer size");
    T result{};
    std::memcpy(&result, &address, sizeof(result));
    return result;
}

template <typename T>
FARPROC FunctionToFarProc(T function) noexcept {
    static_assert(std::is_pointer_v<T>, "T must be a function pointer");
    static_assert(sizeof(T) == sizeof(FARPROC), "Unexpected function pointer size");
    FARPROC result{};
    std::memcpy(&result, &function, sizeof(result));
    return result;
}

// GetProcAddress 返回 FARPROC（Win32 的占位函数签名）。直接 reinterpret_cast
// 到真实 cdecl 函数指针会触发 MinGW -Wcast-function-type。Windows ABI 保证
// 数据地址宽度一致，这里用按位复制保留地址而不执行不兼容函数类型转换。
template <typename T>
bool ResolveProc(HMODULE module, const char* name, T& target) noexcept {
    static_assert(std::is_pointer_v<T>, "ResolveProc target must be a pointer");
    FARPROC address = GetProcAddress(module, name);
    if (address == nullptr) {
        target = nullptr;
        return false;
    }
    static_assert(sizeof(target) == sizeof(address), "Unexpected Windows function pointer size");
    std::memcpy(&target, &address, sizeof(target));
    return true;
}

// 解析按序号导出的 Win32 函数。与名称版本使用同一套按位复制，避免
// MinGW 对 FARPROC 与具体调用约定函数指针直接转换的警告。
template <typename T>
bool ResolveProcOrdinal(HMODULE module, WORD ordinal, T& target) noexcept {
    static_assert(std::is_pointer_v<T>, "T must be a function pointer");
    static_assert(sizeof(T) == sizeof(FARPROC), "Unexpected function pointer size");
    const FARPROC address = GetProcAddress(module, MAKEINTRESOURCEA(ordinal));
    std::memcpy(&target, &address, sizeof(target));
    return target != nullptr;
}

}  // namespace joyshim
