#pragma once

#include <windows.h>
#ifdef _MSC_VER
// Windows SDK 通过 #pragma deprecated(XInputEnable) 标记该 ABI。代理 DLL
// 必须继续声明并导出它，因此仅在包含 SDK 声明及本头的 ABI 重声明期间
// 局部关闭 C4995；离开本头后立即恢复，不掩盖其他弃用 API。
#pragma warning(push)
#pragma warning(disable : 4995)
#endif
#include <xinput.h>

// 新版 Windows SDK 的标准 XInput API 声明包含 noexcept。
// 代理在重声明和定义时必须使用相同的异常规范，避免 MSVC C2382。
// MinGW 与旧版头桩没有该规范，保持原有 ABI 兼容性。
#if defined(_MSC_VER) || defined(JOYSHIM_TEST_MSVC_XINPUT_SDK)
#define JOYSHIM_XINPUT_SDK_NOEXCEPT noexcept
#else
#define JOYSHIM_XINPUT_SDK_NOEXCEPT
#endif

extern "C" DWORD WINAPI XInputGetState(DWORD, XINPUT_STATE*) JOYSHIM_XINPUT_SDK_NOEXCEPT;
extern "C" DWORD WINAPI XInputGetStateEx(DWORD, XINPUT_STATE*);
extern "C" DWORD WINAPI XInputSetState(DWORD, XINPUT_VIBRATION*) JOYSHIM_XINPUT_SDK_NOEXCEPT;
// 使用私有结构保存扩展能力信息：不同版本的 Windows SDK 与 MinGW-w64
// 可能自行声明同名扩展 API 和另一种结构体指针。公开导出仍由 .def 保持原名。
struct JoyShimXInputCapabilitiesEx {
    XINPUT_CAPABILITIES Capabilities{};
    WORD VendorId{};
    WORD ProductId{};
    WORD VersionNumber{};
    WORD Reserved1{};
    DWORD Reserved2{};
};

extern "C" DWORD WINAPI XInputGetCapabilities(DWORD, DWORD, XINPUT_CAPABILITIES*) JOYSHIM_XINPUT_SDK_NOEXCEPT;
// 内部实现不占用系统头文件中的 XInputGetCapabilitiesEx C 符号。
// .def 会将这个函数映射为外部 XInputGetCapabilitiesEx（序号 108）。
extern "C" DWORD WINAPI JoyShimGetCapabilitiesEx(DWORD, DWORD, DWORD, JoyShimXInputCapabilitiesEx*);
extern "C" void WINAPI XInputEnable(BOOL) JOYSHIM_XINPUT_SDK_NOEXCEPT;
extern "C" DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD, GUID*, GUID*);
extern "C" DWORD WINAPI XInputGetAudioDeviceIds(DWORD, LPWSTR, UINT*, LPWSTR, UINT*) JOYSHIM_XINPUT_SDK_NOEXCEPT;
extern "C" DWORD WINAPI XInputGetBatteryInformation(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*) JOYSHIM_XINPUT_SDK_NOEXCEPT;
extern "C" DWORD WINAPI XInputGetKeystroke(DWORD, DWORD, XINPUT_KEYSTROKE*) JOYSHIM_XINPUT_SDK_NOEXCEPT;
extern "C" DWORD WINAPI XInputWaitForGuideButton(DWORD, DWORD, void*);
extern "C" DWORD WINAPI XInputCancelGuideButtonWait(DWORD);
extern "C" DWORD WINAPI XInputPowerOffController(DWORD);

#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace joyshim {

FARPROC ResolveLocalXInputExport(LPCSTR nameOrOrdinal) noexcept;

}  // namespace joyshim
