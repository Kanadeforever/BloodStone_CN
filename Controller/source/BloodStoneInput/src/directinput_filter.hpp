#pragma once

#include <windows.h>

namespace joyshim {

// DirectInput8Create 的精确 ABI 包装。只有配置明确启用
// NativeInput.HideDirectInput 时才从枚举结果中隐藏匹配设备；其余接口、
// 键盘、鼠标和未匹配设备均透明转发到系统 dinput8.dll。
// 所有代理入口用于早期 IAT/动态解析接管的统一入口。它先读取当前游戏
// 配置；只有项目级或手柄子集合实际请求 DirectInput 去重/跟踪时才进入
// COM 包装层，否则直接透明转发系统 DirectInput8Create。
HRESULT WINAPI ConfiguredDirectInput8Create(
    HINSTANCE instance,
    DWORD version,
    const GUID& interfaceId,
    void** output,
    void* outer) noexcept;

HRESULT WINAPI FilteredDirectInput8Create(
    HINSTANCE instance,
    DWORD version,
    const GUID& interfaceId,
    void** output,
    void* outer) noexcept;

}  // namespace joyshim
