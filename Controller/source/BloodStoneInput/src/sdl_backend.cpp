#include "sdl_backend.hpp"

#include <cwchar>

#include "win32_proc.hpp"

namespace joyshim {
namespace {

void SetDetail(wchar_t* detail, std::size_t count, const wchar_t* text) noexcept {
    if (detail == nullptr || count == 0) return;
    const wchar_t* source = text != nullptr ? text : L"";
    const std::size_t sourceLength = std::wcslen(source);
    const std::size_t copyLength = sourceLength < count - 1 ? sourceLength : count - 1;
    if (copyLength > 0) std::wmemmove(detail, source, copyLength);
    detail[copyLength] = L'\0';
}


}  // namespace

bool SdlBackend::Resolve(HMODULE module) noexcept {
#if defined(JOYSHIM_SDL_STATIC)
    // 直接保存静态库的函数地址，不加载 SDL3.dll，也不需要其导入库。
    // 所有入口由 SDL3::SDL3-static 提供，编译器会核对官方 C ABI。
    static_cast<void>(module);
    sdlSetHint_ = &SDL_SetHint;
    sdlSetHintWithPriority_ = &SDL_SetHintWithPriority;
    sdlGetHint_ = &SDL_GetHint;
    sdlInit_ = &SDL_InitSubSystem;
    sdlQuit_ = &SDL_QuitSubSystem;
    sdlGetGamepads_ = &SDL_GetGamepads;
    sdlGetJoysticks_ = &SDL_GetJoysticks;
    sdlIsGamepad_ = &SDL_IsGamepad;
    sdlJoystickNameForId_ = &SDL_GetJoystickNameForID;
    sdlJoystickPathForId_ = &SDL_GetJoystickPathForID;
    sdlJoystickVendorForId_ = &SDL_GetJoystickVendorForID;
    sdlJoystickProductForId_ = &SDL_GetJoystickProductForID;
    sdlJoystickGuidForId_ = &SDL_GetJoystickGUIDForID;
    sdlGamepadGuidForId_ = &SDL_GetGamepadGUIDForID;
    sdlGuidToString_ = &SDL_GUIDToString;
    sdlOpen_ = &SDL_OpenGamepad;
    sdlClose_ = &SDL_CloseGamepad;
    sdlConnected_ = &SDL_GamepadConnected;
    sdlSetEventsEnabled_ = &SDL_SetGamepadEventsEnabled;
    sdlSetJoystickEventsEnabled_ = &SDL_SetJoystickEventsEnabled;
    sdlUpdate_ = &SDL_UpdateGamepads;
    sdlButton_ = &SDL_GetGamepadButton;
    sdlAxis_ = &SDL_GetGamepadAxis;
    sdlRumble_ = &SDL_RumbleGamepad;
    sdlFree_ = &SDL_free;
    sdlVendor_ = &SDL_GetGamepadVendor;
    sdlProduct_ = &SDL_GetGamepadProduct;
    sdlProductVersion_ = &SDL_GetGamepadProductVersion;
    sdlFirmwareVersion_ = &SDL_GetGamepadFirmwareVersion;
    sdlSerial_ = &SDL_GetGamepadSerial;
    sdlType_ = &SDL_GetGamepadType;
    sdlConnection_ = &SDL_GetGamepadConnectionState;
    sdlAddMappings_ = &SDL_AddGamepadMappingsFromFile;
    sdlError_ = &SDL_GetError;
    sdlVersion_ = &SDL_GetVersion;
    sdlName_ = &SDL_GetGamepadName;
    sdlPath_ = &SDL_GetGamepadPath;
    sdlMapping_ = &SDL_GetGamepadMapping;
    sdlJoystick_ = &SDL_GetGamepadJoystick;
    sdlNumAxes_ = &SDL_GetNumJoystickAxes;
    sdlRawAxis_ = &SDL_GetJoystickAxis;
    sdlNumButtons_ = &SDL_GetNumJoystickButtons;
    sdlRawButton_ = &SDL_GetJoystickButton;
    sdlNumHats_ = &SDL_GetNumJoystickHats;
#else
    ResolveProc(module, "SDL_SetHint", sdlSetHint_);
    ResolveProc(module, "SDL_SetHintWithPriority", sdlSetHintWithPriority_);
    ResolveProc(module, "SDL_GetHint", sdlGetHint_);
    ResolveProc(module, "SDL_InitSubSystem", sdlInit_);
    ResolveProc(module, "SDL_QuitSubSystem", sdlQuit_);
    ResolveProc(module, "SDL_GetGamepads", sdlGetGamepads_);
    ResolveProc(module, "SDL_GetJoysticks", sdlGetJoysticks_);
    ResolveProc(module, "SDL_IsGamepad", sdlIsGamepad_);
    ResolveProc(module, "SDL_GetJoystickNameForID", sdlJoystickNameForId_);
    ResolveProc(module, "SDL_GetJoystickPathForID", sdlJoystickPathForId_);
    ResolveProc(module, "SDL_GetJoystickVendorForID", sdlJoystickVendorForId_);
    ResolveProc(module, "SDL_GetJoystickProductForID", sdlJoystickProductForId_);
    ResolveProc(module, "SDL_GetJoystickGUIDForID", sdlJoystickGuidForId_);
    ResolveProc(module, "SDL_GetGamepadGUIDForID", sdlGamepadGuidForId_);
    ResolveProc(module, "SDL_GUIDToString", sdlGuidToString_);
    ResolveProc(module, "SDL_OpenGamepad", sdlOpen_);
    ResolveProc(module, "SDL_CloseGamepad", sdlClose_);
    ResolveProc(module, "SDL_GamepadConnected", sdlConnected_);
    ResolveProc(module, "SDL_SetGamepadEventsEnabled", sdlSetEventsEnabled_);
    ResolveProc(module, "SDL_SetJoystickEventsEnabled", sdlSetJoystickEventsEnabled_);
    ResolveProc(module, "SDL_UpdateGamepads", sdlUpdate_);
    ResolveProc(module, "SDL_GetGamepadButton", sdlButton_);
    ResolveProc(module, "SDL_GetGamepadAxis", sdlAxis_);
    ResolveProc(module, "SDL_RumbleGamepad", sdlRumble_);
    ResolveProc(module, "SDL_free", sdlFree_);
    ResolveProc(module, "SDL_GetGamepadVendor", sdlVendor_);
    ResolveProc(module, "SDL_GetGamepadProduct", sdlProduct_);
    ResolveProc(module, "SDL_GetGamepadProductVersion", sdlProductVersion_);
    ResolveProc(module, "SDL_GetGamepadFirmwareVersion", sdlFirmwareVersion_);
    ResolveProc(module, "SDL_GetGamepadSerial", sdlSerial_);
    ResolveProc(module, "SDL_GetGamepadType", sdlType_);
    ResolveProc(module, "SDL_GetGamepadConnectionState", sdlConnection_);
    ResolveProc(module, "SDL_AddGamepadMappingsFromFile", sdlAddMappings_);
    ResolveProc(module, "SDL_GetError", sdlError_);
    ResolveProc(module, "SDL_GetVersion", sdlVersion_);
    ResolveProc(module, "SDL_GetGamepadName", sdlName_);
    ResolveProc(module, "SDL_GetGamepadPath", sdlPath_);
    ResolveProc(module, "SDL_GetGamepadMapping", sdlMapping_);
    ResolveProc(module, "SDL_GetGamepadJoystick", sdlJoystick_);
    ResolveProc(module, "SDL_GetNumJoystickAxes", sdlNumAxes_);
    ResolveProc(module, "SDL_GetJoystickAxis", sdlRawAxis_);
    ResolveProc(module, "SDL_GetNumJoystickButtons", sdlNumButtons_);
    ResolveProc(module, "SDL_GetJoystickButton", sdlRawButton_);
    ResolveProc(module, "SDL_GetNumJoystickHats", sdlNumHats_);

#endif

    return sdlSetHint_ != nullptr && sdlInit_ != nullptr && sdlQuit_ != nullptr && sdlGetGamepads_ != nullptr && sdlOpen_ != nullptr &&
        sdlClose_ != nullptr && sdlConnected_ != nullptr && sdlUpdate_ != nullptr &&
        sdlButton_ != nullptr && sdlAxis_ != nullptr && sdlFree_ != nullptr;
}

bool SdlBackend::Initialize(
    const RuntimeConfig& config,
    wchar_t* detail,
    std::size_t detailCount) noexcept {
    Shutdown();

#if defined(JOYSHIM_SDL_STATIC)
    // 静态链接时 SDL3 已编入代理 DLL，不需要通过配置路径寻找外部动态库。
    // 显式标记参数已处理，避免严格 MinGW 编译把未使用参数视为错误。
    static_cast<void>(config);
    if (!Resolve(nullptr)) {
        SetDetail(detail, detailCount, L"静态 SDL3 控制器接口不可用。");
        return false;
    }
#else
    wchar_t path[MAX_PATH]{};
    const std::size_t configLength = std::wcslen(config.tomlPath);
    if (configLength == 0 || configLength >= MAX_PATH) {
        SetDetail(detail, detailCount, L"无法确定 SDL3.dll 路径。");
        return false;
    }
    std::wmemcpy(path, config.tomlPath, configLength + 1);
    wchar_t* slash = std::wcsrchr(path, L'\\');
    if (slash == nullptr) {
        SetDetail(detail, detailCount, L"无法确定 SDL3.dll 路径。");
        return false;
    }
    ++slash;
    const wchar_t fileName[] = L"SDL3.dll";
    const std::size_t prefixLength = static_cast<std::size_t>(slash - path);
    if (prefixLength + std::size(fileName) > MAX_PATH) {
        SetDetail(detail, detailCount, L"SDL3.dll 路径过长。");
        return false;
    }
    std::wmemcpy(slash, fileName, std::size(fileName));
    module_ = LoadLibraryW(path);
    if (module_ == nullptr) {
        SetDetail(detail, detailCount, L"无法加载同目录 SDL3.dll。");
        return false;
    }
    if (!Resolve(module_)) {
        SetDetail(detail, detailCount, L"SDL3.dll 缺少必需的 SDL3 Gamepad API。");
        Shutdown();
        return false;
    }
#endif
    // 本 DLL 本身提供 XInput，因此必须关闭 SDL 的 XInput 控制器后端，
    // 防止 SDL 通过同名代理递归读回自身。Xbox 控制器在 Windows 上可能
    // 通过普通 HID、GIP、官方无线适配器、GameInput 或 WGI 暴露；只启用
    // HIDAPI_XBOX 无法覆盖全部连接方式。因此在保持 XInput 关闭的同时，
    // 显式启用 SDL3 的 Xbox One/GIP、GameInput、WGI 与检测线程。
    // RAWINPUT 对 XInput 的反向关联也必须关闭，否则它仍可能读回本代理。
    // 所有提示必须在 SDL_InitSubSystem(SDL_INIT_GAMEPAD) 前设置。
    const auto setRequiredHint = [&](const char* name, const char* value) noexcept {
        if (sdlSetHintWithPriority_ != nullptr) {
            return sdlSetHintWithPriority_(name, value, SDL_HINT_OVERRIDE);
        }
        return sdlSetHint_(name, value);
    };
    struct RequiredHint {
        const char* name;
        const char* value;
        const wchar_t* failure;
    };
    static constexpr RequiredHint requiredHints[] = {
        {"SDL_XINPUT_ENABLED", "0", L"无法关闭 SDL XInput 后端，已阻止可能的代理递归。"},
        {"SDL_JOYSTICK_RAWINPUT_CORRELATE_XINPUT", "0", L"无法关闭 RawInput/XInput 关联，已阻止可能的代理递归。"},
        {"SDL_JOYSTICK_HIDAPI", "1", L"无法启用 SDL HIDAPI 控制器后端。"},
        {"SDL_JOYSTICK_HIDAPI_XBOX", "1", L"无法启用 SDL Xbox HIDAPI 控制器后端。"},
        {"SDL_JOYSTICK_HIDAPI_XBOX_ONE", "1", L"无法启用 SDL Xbox One HIDAPI 控制器后端。"},
        {"SDL_JOYSTICK_HIDAPI_GIP", "1", L"无法启用 SDL Xbox GIP 控制器后端。"},
        {"SDL_JOYSTICK_GAMEINPUT", "1", L"无法启用 SDL GameInput 控制器后端。"},
        {"SDL_JOYSTICK_WGI", "1", L"无法启用 SDL Windows.Gaming.Input 控制器后端。"},
        {"SDL_JOYSTICK_THREAD", "1", L"无法启用 SDL Windows 控制器检测线程。"},
    };
    for (const RequiredHint& hint : requiredHints) {
        if (!setRequiredHint(hint.name, hint.value)) {
            SetDetail(detail, detailCount, hint.failure);
            Shutdown();
            return false;
        }
    }
    if (!sdlInit_(SDL_INIT_GAMEPAD)) {
        wchar_t message[512]{};
        const char* error = sdlError_ != nullptr ? sdlError_() : nullptr;
        wchar_t wideError[384]{};
        if (error != nullptr) MultiByteToWideChar(CP_UTF8, 0, error, -1, wideError, 384);
        std::swprintf(message, 512, L"SDL3 Gamepad 子系统初始化失败：%ls",
            wideError[0] != L'\0' ? wideError : L"未知错误");
        SetDetail(detail, detailCount, message);
        Shutdown();
        return false;
    }

    if (sdlSetEventsEnabled_ != nullptr) sdlSetEventsEnabled_(false);
    if (sdlSetJoystickEventsEnabled_ != nullptr) sdlSetJoystickEventsEnabled_(false);
    if (sdlVersion_ != nullptr) {
        const int version = sdlVersion_();
        std::swprintf(versionText_, 32, L"%d.%d.%d",
            version / 1000000, (version / 1000) % 1000, version % 1000);
    }
    initialized_ = true;
    wchar_t message[512]{};
    std::swprintf(
        message,
        512,
        L"SDL3 后端初始化成功，版本=%ls；XInput=关闭，RawInput/XInput关联=关闭，HIDAPI Xbox/GIP=开启，GameInput=开启，WGI=开启，Joystick线程=开启。%ls",
        versionText_,
        (sdlVersion_ != nullptr && sdlVersion_() < 3004014)
            ? L" 警告：当前 SDL3 早于 3.4.14，可能缺少 GameInput 下的 Xbox 设备发现修复，建议替换为 SDL3 3.4.14。"
            : L"");
    SetDetail(detail, detailCount, message);
    return true;
}

void SdlBackend::Shutdown() noexcept {
    if (initialized_ && sdlQuit_ != nullptr) sdlQuit_(SDL_INIT_GAMEPAD);
    initialized_ = false;
    if (module_ != nullptr) FreeLibrary(module_);
    *this = SdlBackend{};
}

bool SdlBackend::Enumerate(
    std::array<SdlDeviceId, MaxEnumeratedGamepads>& ids,
    std::size_t& count) noexcept {
    count = 0;
    if (!initialized_) return false;
    int nativeCount = 0;
    SDL_JoystickID* nativeIds = sdlGetGamepads_(&nativeCount);
    if (nativeIds == nullptr) return nativeCount == 0;
    const std::size_t safeCount = nativeCount > 0
        ? (static_cast<std::size_t>(nativeCount) < ids.size()
            ? static_cast<std::size_t>(nativeCount) : ids.size()) : 0;
    for (std::size_t index = 0; index < safeCount; ++index) ids[index] = nativeIds[index];
    count = safeCount;
    sdlFree_(nativeIds);
    return true;
}


bool SdlBackend::EnumerateJoysticks(
    std::array<SdlDeviceId, MaxEnumeratedGamepads>& ids,
    std::size_t& count) noexcept {
    count = 0;
    if (!initialized_ || sdlGetJoysticks_ == nullptr) return false;
    int nativeCount = 0;
    SDL_JoystickID* nativeIds = sdlGetJoysticks_(&nativeCount);
    if (nativeIds == nullptr) return nativeCount == 0;
    const std::size_t safeCount = nativeCount > 0
        ? (static_cast<std::size_t>(nativeCount) < ids.size()
            ? static_cast<std::size_t>(nativeCount) : ids.size()) : 0;
    for (std::size_t index = 0; index < safeCount; ++index) ids[index] = nativeIds[index];
    count = safeCount;
    sdlFree_(nativeIds);
    return true;
}

bool SdlBackend::IsGamepad(SdlDeviceId id) noexcept {
    return sdlIsGamepad_ != nullptr && sdlIsGamepad_(id);
}

const char* SdlBackend::GetJoystickNameForId(SdlDeviceId id) noexcept {
    return sdlJoystickNameForId_ != nullptr ? sdlJoystickNameForId_(id) : nullptr;
}

const char* SdlBackend::GetJoystickPathForId(SdlDeviceId id) noexcept {
    return sdlJoystickPathForId_ != nullptr ? sdlJoystickPathForId_(id) : nullptr;
}

std::uint16_t SdlBackend::GetJoystickVendorForId(SdlDeviceId id) noexcept {
    return sdlJoystickVendorForId_ != nullptr ? sdlJoystickVendorForId_(id) : 0;
}

std::uint16_t SdlBackend::GetJoystickProductForId(SdlDeviceId id) noexcept {
    return sdlJoystickProductForId_ != nullptr ? sdlJoystickProductForId_(id) : 0;
}

bool SdlBackend::GetJoystickGuidForId(
    SdlDeviceId id,
    char* output,
    std::size_t outputCount) noexcept {
    if (output == nullptr || outputCount == 0) return false;
    output[0] = '\0';
    if (sdlJoystickGuidForId_ == nullptr || sdlGuidToString_ == nullptr || outputCount > 0x7FFFFFFFu) {
        return false;
    }
    sdlGuidToString_(sdlJoystickGuidForId_(id), output, static_cast<int>(outputCount));
    return output[0] != '\0';
}

SdlGamepadHandle SdlBackend::OpenGamepad(SdlDeviceId id) noexcept {
    return initialized_ ? sdlOpen_(id) : nullptr;
}
void SdlBackend::CloseGamepad(SdlGamepadHandle gamepad) noexcept {
    if (gamepad != nullptr && sdlClose_ != nullptr) sdlClose_(static_cast<SDL_Gamepad*>(gamepad));
}
bool SdlBackend::GamepadConnected(SdlGamepadHandle gamepad) noexcept {
    return gamepad != nullptr && sdlConnected_ != nullptr && sdlConnected_(static_cast<SDL_Gamepad*>(gamepad));
}
void SdlBackend::UpdateGamepads() noexcept { if (sdlUpdate_ != nullptr) sdlUpdate_(); }
bool SdlBackend::GetGamepadButton(SdlGamepadHandle gamepad, int button) noexcept {
    return gamepad != nullptr && button >= 0 && button < SDL_GAMEPAD_BUTTON_COUNT &&
        sdlButton_(static_cast<SDL_Gamepad*>(gamepad), static_cast<SDL_GamepadButton>(button));
}
std::int16_t SdlBackend::GetGamepadAxis(SdlGamepadHandle gamepad, int axis) noexcept {
    if (gamepad == nullptr || axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT) return 0;
    return sdlAxis_(static_cast<SDL_Gamepad*>(gamepad), static_cast<SDL_GamepadAxis>(axis));
}
bool SdlBackend::RumbleGamepad(SdlGamepadHandle gamepad, std::uint16_t low,
    std::uint16_t high, std::uint32_t durationMs) noexcept {
    return gamepad != nullptr && sdlRumble_ != nullptr &&
        sdlRumble_(static_cast<SDL_Gamepad*>(gamepad), low, high, durationMs);
}
std::uint16_t SdlBackend::GetGamepadVendor(SdlGamepadHandle gamepad) noexcept {
    return gamepad != nullptr && sdlVendor_ != nullptr ? sdlVendor_(static_cast<SDL_Gamepad*>(gamepad)) : 0;
}
std::uint16_t SdlBackend::GetGamepadProduct(SdlGamepadHandle gamepad) noexcept {
    return gamepad != nullptr && sdlProduct_ != nullptr ? sdlProduct_(static_cast<SDL_Gamepad*>(gamepad)) : 0;
}

std::uint16_t SdlBackend::GetGamepadProductVersion(SdlGamepadHandle gamepad) noexcept {
    return gamepad != nullptr && sdlProductVersion_ != nullptr
        ? sdlProductVersion_(static_cast<SDL_Gamepad*>(gamepad)) : 0;
}
std::uint16_t SdlBackend::GetGamepadFirmwareVersion(SdlGamepadHandle gamepad) noexcept {
    return gamepad != nullptr && sdlFirmwareVersion_ != nullptr
        ? sdlFirmwareVersion_(static_cast<SDL_Gamepad*>(gamepad)) : 0;
}
int SdlBackend::GetGamepadType(SdlGamepadHandle gamepad) noexcept {
    return gamepad != nullptr && sdlType_ != nullptr
        ? static_cast<int>(sdlType_(static_cast<SDL_Gamepad*>(gamepad)))
        : static_cast<int>(SDL_GAMEPAD_TYPE_UNKNOWN);
}
const char* SdlBackend::GetGamepadSerial(SdlGamepadHandle gamepad) noexcept {
    return gamepad != nullptr && sdlSerial_ != nullptr
        ? sdlSerial_(static_cast<SDL_Gamepad*>(gamepad)) : nullptr;
}
bool SdlBackend::GetGamepadGuidForId(
    SdlDeviceId id,
    char* output,
    std::size_t outputCount) noexcept {
    if (output == nullptr || outputCount == 0) return false;
    output[0] = '\0';
    if (sdlGamepadGuidForId_ == nullptr || sdlGuidToString_ == nullptr || outputCount > 0x7FFFFFFFu) {
        return false;
    }
    sdlGuidToString_(sdlGamepadGuidForId_(id), output, static_cast<int>(outputCount));
    return output[0] != '\0';
}
ConnectionKind SdlBackend::GetConnectionKind(SdlGamepadHandle gamepad) noexcept {
    if (gamepad == nullptr || sdlConnection_ == nullptr) return ConnectionKind::Unknown;
    const auto state = sdlConnection_(static_cast<SDL_Gamepad*>(gamepad));
    if (state == SDL_JOYSTICK_CONNECTION_WIRED) return ConnectionKind::Wired;
    if (state == SDL_JOYSTICK_CONNECTION_WIRELESS) return ConnectionKind::Wireless;
    return ConnectionKind::Unknown;
}
const char* SdlBackend::GetGamepadName(SdlGamepadHandle gamepad) noexcept {
    return gamepad != nullptr && sdlName_ != nullptr ? sdlName_(static_cast<SDL_Gamepad*>(gamepad)) : nullptr;
}
const char* SdlBackend::GetGamepadPath(SdlGamepadHandle gamepad) noexcept {
    return gamepad != nullptr && sdlPath_ != nullptr ? sdlPath_(static_cast<SDL_Gamepad*>(gamepad)) : nullptr;
}
char* SdlBackend::GetGamepadMapping(SdlGamepadHandle gamepad) noexcept {
    return gamepad != nullptr && sdlMapping_ != nullptr ? sdlMapping_(static_cast<SDL_Gamepad*>(gamepad)) : nullptr;
}
SdlJoystickHandle SdlBackend::GetGamepadJoystick(SdlGamepadHandle gamepad) noexcept {
    return gamepad != nullptr && sdlJoystick_ != nullptr ? sdlJoystick_(static_cast<SDL_Gamepad*>(gamepad)) : nullptr;
}
int SdlBackend::GetNumJoystickAxes(SdlJoystickHandle joystick) noexcept {
    return joystick != nullptr && sdlNumAxes_ != nullptr ? sdlNumAxes_(static_cast<SDL_Joystick*>(joystick)) : 0;
}
std::int16_t SdlBackend::GetJoystickAxis(SdlJoystickHandle joystick, int axis) noexcept {
    return joystick != nullptr && sdlRawAxis_ != nullptr ? sdlRawAxis_(static_cast<SDL_Joystick*>(joystick), axis) : 0;
}
int SdlBackend::GetNumJoystickButtons(SdlJoystickHandle joystick) noexcept {
    return joystick != nullptr && sdlNumButtons_ != nullptr ? sdlNumButtons_(static_cast<SDL_Joystick*>(joystick)) : 0;
}
int SdlBackend::GetNumJoystickHats(SdlJoystickHandle joystick) noexcept {
    return joystick != nullptr && sdlNumHats_ != nullptr
        ? sdlNumHats_(static_cast<SDL_Joystick*>(joystick)) : 0;
}
bool SdlBackend::GetJoystickButton(SdlJoystickHandle joystick, int button) noexcept {
    return joystick != nullptr && sdlRawButton_ != nullptr && sdlRawButton_(static_cast<SDL_Joystick*>(joystick), button);
}
int SdlBackend::AddGamepadMappingsFromFile(const char* path) noexcept {
    return sdlAddMappings_ != nullptr ? sdlAddMappings_(path) : -1;
}
const char* SdlBackend::GetError() noexcept { return sdlError_ != nullptr ? sdlError_() : nullptr; }
void SdlBackend::FreeMemory(void* memory) noexcept { if (memory != nullptr && sdlFree_ != nullptr) sdlFree_(memory); }

}  // namespace joyshim
