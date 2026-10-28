#pragma once

#include <windows.h>

#include <cstdint>

namespace joyshim {

struct RuntimeConfig;

// 当前文件实际使用的代理入口。内部代码与日志一律使用 JoyShim 名称，
// 对外文件名只决定系统 API 转发方式。
enum class ProxyEntryKind {
    Unknown,
    XInput11,
    XInput12,
    XInput13,
    XInput14,
    XInput910,
    DInput8,
    WinMM,
};

struct ProxyHookSnapshot {
    std::uint32_t modulesVisited{};
    std::uint32_t normalImportDescriptors{};
    std::uint32_t delayImportDescriptors{};
    std::uint32_t xinputSlotsPatched{};
    std::uint32_t directInputSlotsPatched{};
    std::uint32_t getProcAddressSlotsPatched{};
    std::uint32_t patchFailures{};
    std::uint32_t dynamicXInputRedirects{};
    std::uint32_t dynamicDirectInputRedirects{};
    std::uint32_t directInputCreateCalls{};
    std::uint32_t directInputDevicesHidden{};
};

// DllMain 先记录模块、入口名称与游戏根目录。该步骤不解析配置、不加载
// SDL3，也不遍历后加载模块。
void ProxyEntryProcessAttach(HMODULE module) noexcept;

// 工作线程已离开 loader lock 后，对主 EXE“按名称普通导入”建立最小
// 接管：XInput、DirectInput8Create 与 GetProcAddress。严禁从 DllMain 调用。
void ProxyEntryInstallEarlyHooks() noexcept;

// 排队一次性延迟启动线程。允许从 DLL_PROCESS_ATTACH 调用，但绝不
// 在 DllMain 等待线程。导入表写入、配置解析和 SDL 初始化只在工作线程执行。
void ProxyEntryQueueRuntimeBootstrap() noexcept;

// 配置解析完成后启用当前代理入口需要的接管链：
// - XInput 入口不重复改写宿主已经指向 JoyShim 的 XInput IAT；
// - dinput8/winmm 重定向宿主 XInput 普通/动态解析；
// - 配置要求时重定向 DirectInput8Create；
// - 初始阶段只处理主程序普通 IAT，后加载模块由输入线程节流处理。
void ProxyEntryConfigureHooks(
    const RuntimeConfig& config,
    bool blockDirectInput) noexcept;

// 独立 SDL 输入线程定期调用。用于处理启动后加载的游戏模块；函数内部有
// 节流和幂等保护，不得从 DllMain 或加载通知回调调用。
void ProxyEntryRefreshHooks(const RuntimeConfig& config) noexcept;

// DirectInput 包装层用于提供可验证的实际执行统计。
void ProxyEntryNotifyDirectInputCreate() noexcept;
void ProxyEntryNotifyDirectInputDeviceHidden() noexcept;

void ProxyEntryProcessDetach() noexcept;
HMODULE ProxyEntryModule() noexcept;
ProxyEntryKind ProxyEntryCurrentKind() noexcept;
const wchar_t* ProxyEntryCurrentName() noexcept;
bool ProxyEntrySupportsDirectInputFilter() noexcept;
ProxyHookSnapshot ProxyEntryHookSnapshot() noexcept;

}  // namespace joyshim
