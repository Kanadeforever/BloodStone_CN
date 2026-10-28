#include <windows.h>

#ifdef _MSC_VER
#pragma warning(disable : 4995)
#endif
#include <xinput.h>

#include <conio.h>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <cstring>

#include "win32_proc.hpp"

namespace {

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

struct AxisRange {
    SHORT minimum{};
    SHORT maximum{};
    bool initialized{};
};

void UpdateRange(AxisRange& range, SHORT value) noexcept {
    if (!range.initialized) {
        range.minimum = value;
        range.maximum = value;
        range.initialized = true;
        return;
    }
    if (value < range.minimum) range.minimum = value;
    if (value > range.maximum) range.maximum = value;
}

const char* Direction(SHORT x, SHORT y) noexcept {
    constexpr SHORT threshold = 2000;
    if (x < -threshold) {
        if (y > threshold) return "Left-Up";
        if (y < -threshold) return "Left-Down";
        return "Left";
    }
    if (x > threshold) {
        if (y > threshold) return "Right-Up";
        if (y < -threshold) return "Right-Down";
        return "Right";
    }
    if (y > threshold) return "Up";
    if (y < -threshold) return "Down";
    return "Center";
}

void CursorHome() noexcept {
    SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), COORD{0, 0});
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

bool IsAlternativeEntry(const wchar_t* path) noexcept {
    if (path == nullptr) return false;
    const wchar_t* slash = std::wcsrchr(path, L'\\');
    const wchar_t* name = slash != nullptr ? slash + 1 : path;
    return EqualsIgnoreCase(name, L"dinput8.dll") ||
        EqualsIgnoreCase(name, L"winmm.dll");
}

bool BuildSystemPath(const wchar_t* fileName, wchar_t* output, DWORD count) noexcept {
    if (fileName == nullptr || output == nullptr || count == 0) return false;
    const UINT length = GetSystemDirectoryW(output, count);
    if (length == 0 || length >= count) return false;
    const std::size_t nameLength = std::wcslen(fileName);
    if (static_cast<std::size_t>(length) + 1 + nameLength + 1 > count) return false;
    output[length] = L'\\';
    std::wmemcpy(output + length + 1, fileName, nameLength + 1);
    return true;
}

bool BuildSiblingPath(const wchar_t* fileName, wchar_t* output, DWORD count) noexcept {
    if (fileName == nullptr || output == nullptr || count == 0) return false;
    const DWORD length = GetModuleFileNameW(nullptr, output, count);
    if (length == 0 || length >= count) return false;
    wchar_t* slash = std::wcsrchr(output, L'\\');
    if (slash == nullptr) return false;
    const std::size_t directoryLength = static_cast<std::size_t>(slash - output + 1);
    const std::size_t nameLength = std::wcslen(fileName);
    if (directoryLength + nameLength + 1 > count) return false;
    std::wmemcpy(output + directoryLength, fileName, nameLength + 1);
    return true;
}

std::uintptr_t PointerValue(FARPROC pointer) noexcept {
    std::uintptr_t value{};
    static_assert(sizeof(value) == sizeof(pointer), "Unexpected pointer size");
    std::memcpy(&value, &pointer, sizeof(value));
    return value;
}

void PrintButtons(WORD buttons) noexcept {
    struct Entry { WORD mask; const char* name; };
    static constexpr Entry entries[] = {
        {XINPUT_GAMEPAD_DPAD_UP, "DpadUp"},
        {XINPUT_GAMEPAD_DPAD_DOWN, "DpadDown"},
        {XINPUT_GAMEPAD_DPAD_LEFT, "DpadLeft"},
        {XINPUT_GAMEPAD_DPAD_RIGHT, "DpadRight"},
        {XINPUT_GAMEPAD_START, "Start"},
        {XINPUT_GAMEPAD_BACK, "Back"},
        {XINPUT_GAMEPAD_LEFT_THUMB, "LeftThumb"},
        {XINPUT_GAMEPAD_RIGHT_THUMB, "RightThumb"},
        {XINPUT_GAMEPAD_LEFT_SHOULDER, "LeftShoulder"},
        {XINPUT_GAMEPAD_RIGHT_SHOULDER, "RightShoulder"},
        {0x0400, "Guide"},
        {XINPUT_GAMEPAD_A, "A"},
        {XINPUT_GAMEPAD_B, "B"},
        {XINPUT_GAMEPAD_X, "X"},
        {XINPUT_GAMEPAD_Y, "Y"},
    };
    for (const Entry& entry : entries) {
        if ((buttons & entry.mask) != 0) std::printf(" %s", entry.name);
    }
}

void PrintState(const char* label, DWORD result, const XINPUT_STATE& state) noexcept {
    if (result != ERROR_SUCCESS) {
        std::printf("%-10s result=%lu\n", label, static_cast<unsigned long>(result));
        return;
    }
    std::printf(
        "%-10s packet=%10lu  LX=%6d LY=%6d RX=%6d RY=%6d LT=%3u RT=%3u direction=%-10s buttons=%04X",
        label,
        static_cast<unsigned long>(state.dwPacketNumber),
        state.Gamepad.sThumbLX,
        state.Gamepad.sThumbLY,
        state.Gamepad.sThumbRX,
        state.Gamepad.sThumbRY,
        state.Gamepad.bLeftTrigger,
        state.Gamepad.bRightTrigger,
        Direction(state.Gamepad.sThumbRX, state.Gamepad.sThumbRY),
        state.Gamepad.wButtons);
    PrintButtons(state.Gamepad.wButtons);
    std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    wchar_t requested[MAX_PATH]{L"xinput1_3.dll"};
    if (argc >= 2 && argv[1] != nullptr) {
        if (MultiByteToWideChar(CP_ACP, 0, argv[1], -1, requested, MAX_PATH) <= 0) return 1;
    }
    wchar_t path[MAX_PATH]{};
    if ((std::wcschr(requested, L'\\') != nullptr || std::wcschr(requested, L'/') != nullptr)) {
        const std::size_t length = std::wcslen(requested);
        if (length + 1 > MAX_PATH) return 1;
        std::wmemcpy(path, requested, length + 1);
    } else if (!BuildSiblingPath(requested, path, MAX_PATH)) {
        std::fprintf(stderr, "无法构造代理 DLL 路径。\n");
        return 1;
    }

    HMODULE module = LoadLibraryW(path);
    if (module == nullptr) {
        std::fwprintf(stderr, L"无法加载：%ls，错误=%lu\n", path, GetLastError());
        std::fprintf(stderr, "请把同架构 JoyShim 代理 DLL 放在本程序旁边；代理 DLL 同名的 TOML 可选，SDL3 已静态链接。\n");
        return 2;
    }

    XInputGetStateFn byName{};
    XInputGetStateFn byOrdinal2{};
    XInputGetStateFn byOrdinal100{};
    joyshim::ResolveProc(module, "XInputGetState", byName);
    joyshim::ResolveProcOrdinal(module, 2, byOrdinal2);
    joyshim::ResolveProcOrdinal(module, 100, byOrdinal100);
    if (byName == nullptr || byOrdinal2 == nullptr || byName != byOrdinal2) {
        std::fprintf(stderr, "目标 DLL 的 XInputGetState 名称导出与官方序号 2 不一致。\n");
        FreeLibrary(module);
        return 3;
    }

    bool alternativeHookVerified = false;
    HMODULE systemXInput = nullptr;
    if (IsAlternativeEntry(path)) {
        wchar_t systemPath[MAX_PATH]{};
        if (!BuildSystemPath(L"xinput1_4.dll", systemPath, MAX_PATH)) {
            std::fprintf(stderr, "无法构造系统 XInput 路径。\n");
            FreeLibrary(module);
            return 4;
        }
        systemXInput = LoadLibraryW(systemPath);
        const FARPROC intercepted = systemXInput != nullptr
            ? GetProcAddress(systemXInput, "XInputGetState") : nullptr;
        alternativeHookVerified = intercepted == joyshim::FunctionToFarProc(byName);
        if (!alternativeHookVerified) {
            std::fprintf(stderr, "替代入口已加载，但主程序 GetProcAddress 的 XInput 接管未生效。\n");
            if (systemXInput != nullptr) FreeLibrary(systemXInput);
            FreeLibrary(module);
            return 4;
        }
    }

    AxisRange rx{};
    AxisRange ry{};
    for (;;) {
        XINPUT_STATE named{};
        XINPUT_STATE extended{};
        const DWORD namedResult = byName(0, &named);
        const DWORD extendedResult = byOrdinal100 != nullptr
            ? byOrdinal100(0, &extended)
            : ERROR_PROC_NOT_FOUND;
        if (namedResult == ERROR_SUCCESS) {
            UpdateRange(rx, named.Gamepad.sThumbRX);
            UpdateRange(ry, named.Gamepad.sThumbRY);
        }

        CursorHome();
        std::wprintf(L"JoyShim - XInputStateProbe  |  DLL=%ls\n", path);
        std::printf("ESC 退出，R 清零范围。此工具调用的是成品代理 DLL，不读取 SDL 原始轴。\n");
        if (IsAlternativeEntry(path)) {
            std::printf("替代入口 GetProcAddress 接管：%s\n",
                alternativeHookVerified ? "通过" : "失败");
        }
        std::printf("\n");
        const FARPROC addressByName = GetProcAddress(module, "XInputGetState");
        const FARPROC addressByOrdinal1 = GetProcAddress(module, MAKEINTRESOURCEA(1));
        const FARPROC addressByOrdinal2 = GetProcAddress(module, MAKEINTRESOURCEA(2));
        const FARPROC addressByOrdinal100 = GetProcAddress(module, MAKEINTRESOURCEA(100));
        std::printf("ABI: ordinal1(DllMain)=0x%llX\n",
            static_cast<unsigned long long>(PointerValue(addressByOrdinal1)));
        std::printf("ABI: name(XInputGetState)=0x%llX  ordinal2=0x%llX  %s\n",
            static_cast<unsigned long long>(PointerValue(addressByName)),
            static_cast<unsigned long long>(PointerValue(addressByOrdinal2)),
            addressByName == addressByOrdinal2 ? "MATCH" : "MISMATCH");
        std::printf("ABI: ordinal100(XInputGetStateEx)=0x%llX\n\n",
            static_cast<unsigned long long>(PointerValue(addressByOrdinal100)));
        PrintState("By name", namedResult, named);
        PrintState("Ordinal100", extendedResult, extended);
        std::printf("\nObserved named RX=[%6d,%6d] RY=[%6d,%6d]\n",
            rx.initialized ? rx.minimum : 0,
            rx.initialized ? rx.maximum : 0,
            ry.initialized ? ry.minimum : 0,
            ry.initialized ? ry.maximum : 0);
        if (namedResult == ERROR_SUCCESS) {
            const int ax = named.Gamepad.sThumbRX < 0 ? -named.Gamepad.sThumbRX : named.Gamepad.sThumbRX;
            const int ay = named.Gamepad.sThumbRY < 0 ? -named.Gamepad.sThumbRY : named.Gamepad.sThumbRY;
            const bool diagonal = ax > 4000 && ay > 4000 && (named.Gamepad.sThumbRX == named.Gamepad.sThumbRY || named.Gamepad.sThumbRX == -named.Gamepad.sThumbRY);
            std::printf("Exact diagonal equality: %s\n", diagonal ? "YES" : "no");
        } else {
            std::printf("Exact diagonal equality: unavailable\n");
        }
        std::printf("                                                                                \n");

        if (_kbhit()) {
            const int key = _getch();
            if (key == 27) break;
            if (key == 'r' || key == 'R') {
                rx = {};
                ry = {};
            }
        }
        Sleep(50);
    }

    if (systemXInput != nullptr) FreeLibrary(systemXInput);
    FreeLibrary(module);
    return 0;
}
