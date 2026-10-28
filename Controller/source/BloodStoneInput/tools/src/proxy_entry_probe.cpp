#include <windows.h>
#include <xinput.h>

#include "win32_proc.hpp"

#include <cstdio>
#include <cwchar>
#include <cwctype>

namespace {

const wchar_t* BaseName(const wchar_t* path) noexcept {
    if (path == nullptr) return L"";
    const wchar_t* slash = std::wcsrchr(path, L'\\');
    return slash != nullptr ? slash + 1 : path;
}

bool EqualsIgnoreCase(const wchar_t* left, const wchar_t* right) noexcept {
    if (left == nullptr || right == nullptr) return false;
    while (*left != L'\0' && *right != L'\0') {
        if (std::towlower(*left) != std::towlower(*right)) return false;
        ++left;
        ++right;
    }
    return *left == *right;
}

bool IsXInputName(const wchar_t* name) noexcept {
    return EqualsIgnoreCase(name, L"xinput1_1.dll") ||
        EqualsIgnoreCase(name, L"xinput1_2.dll") ||
        EqualsIgnoreCase(name, L"xinput1_3.dll") ||
        EqualsIgnoreCase(name, L"xinput1_4.dll") ||
        EqualsIgnoreCase(name, L"xinput9_1_0.dll");
}

bool BuildSystemPath(const wchar_t* fileName, wchar_t* output, std::size_t count) noexcept {
    if (fileName == nullptr || output == nullptr || count == 0) return false;
    output[0] = L'\0';
    const UINT length = GetSystemDirectoryW(output, static_cast<UINT>(count));
    if (length == 0 || length >= count) return false;
    const std::size_t nameLength = std::wcslen(fileName);
    if (static_cast<std::size_t>(length) + 1 + nameLength + 1 > count) return false;
    output[length] = L'\\';
    std::wmemcpy(output + length + 1, fileName, nameLength + 1);
    return true;
}

bool ProbeWinMM(HMODULE module) noexcept {
    using Function = DWORD(WINAPI*)();
    const auto function = joyshim::FarProcToFunction<Function>(
        GetProcAddress(module, "timeGetTime"));
    if (function == nullptr) return false;
    const DWORD value = function();
    std::wprintf(
        L"winmm 系统转发：timeGetTime=%lu\n",
        static_cast<unsigned long>(value));
    return true;
}

bool ProbeDInput8(HMODULE module) noexcept {
    using Function = LONG(WINAPI*)();
    const auto function = joyshim::FarProcToFunction<Function>(
        GetProcAddress(module, "DllCanUnloadNow"));
    if (function == nullptr) return false;
    const LONG value = function();
    std::wprintf(
        L"dinput8 系统转发：DllCanUnloadNow=0x%08lX\n",
        static_cast<unsigned long>(value));
    return true;
}

bool ProbeXInput(HMODULE module) noexcept {
    using Function = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    const auto function = joyshim::FarProcToFunction<Function>(
        GetProcAddress(module, "XInputGetState"));
    if (function == nullptr) return false;
    XINPUT_STATE state{};
    const DWORD result = function(0, &state);
    std::wprintf(
        L"JoyShim XInputGetState(0)=%lu，RX=%d，RY=%d\n",
        static_cast<unsigned long>(result),
        static_cast<int>(state.Gamepad.sThumbRX),
        static_cast<int>(state.Gamepad.sThumbRY));
    return result == ERROR_SUCCESS || result == ERROR_DEVICE_NOT_CONNECTED;
}


}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fwprintf(
            stderr,
            L"用法：JoyShimProxyEntryProbe.exe <七种 JoyShim DLL 中的一个完整路径>\n");
        return 2;
    }

    HMODULE module = LoadLibraryW(argv[1]);
    if (module == nullptr) {
        std::fwprintf(
            stderr,
            L"LoadLibraryW 失败：%lu\n",
            static_cast<unsigned long>(GetLastError()));
        return 3;
    }

    const wchar_t* name = BaseName(argv[1]);
    const bool known = IsXInputName(name) ||
        EqualsIgnoreCase(name, L"dinput8.dll") ||
        EqualsIgnoreCase(name, L"winmm.dll");
    if (!known) {
        std::fwprintf(stderr, L"不支持的代理名称：%ls\n", name);
        return 4;
    }

    // 正式发行将 SDL3 静态链接进每个代理 DLL，不再加载独立 SDL3.dll。
    // 因此不能用 GetModuleHandleW("SDL3.dll") 来判定输入线程是否初始化。
    // 本工具只验证代理能够加载、XInput 导出或系统代理转发能够调用。
    // SDL 初始化和零手柄热插拔仍需通过输入诊断及实机测试单独验证。
    std::wprintf(L"SDL3 静态链接：不使用外置 SDL3.dll；初始化状态不由入口探针判定。\n");

    bool forwardingOk = true;
    bool xinputOk = true;
    if (EqualsIgnoreCase(name, L"winmm.dll")) {
        forwardingOk = ProbeWinMM(module);
    } else if (EqualsIgnoreCase(name, L"dinput8.dll")) {
        forwardingOk = ProbeDInput8(module);
    } else {
        xinputOk = ProbeXInput(module);
    }

    std::wprintf(
        L"入口=%ls，入口导出/系统转发=%ls\n",
        name,
        forwardingOk && xinputOk ? L"通过" : L"失败");

    // 七个入口是七个独立 DLL：系统代理不再额外导出 XInput 名称。
    // 它们通过早期 IAT 接管把宿主真正的 XInput 调用重定向到共享核心。
    // 代理运行时拥有独立输入线程，测试进程直接退出，不显式 FreeLibrary。
    return forwardingOk && xinputOk ? 0 : 5;
}
