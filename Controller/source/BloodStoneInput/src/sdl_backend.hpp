#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "sdl3_minimal.hpp"

namespace joyshim {

using SdlDeviceId = std::uint32_t;
using SdlGamepadHandle = void*;
using SdlJoystickHandle = void*;

constexpr std::size_t MaxEnumeratedGamepads = 64;
constexpr int UnifiedGamepadAxisCount = 6;
constexpr int UnifiedGamepadButtonCount = 26;

class SdlBackend {
public:
    bool Initialize(const RuntimeConfig& config, wchar_t* detail, std::size_t detailCount) noexcept;
    void Shutdown() noexcept;

    const wchar_t* Name() const noexcept { return initialized_ ? L"SDL3" : L"未初始化"; }
    const wchar_t* VersionText() const noexcept { return versionText_; }

    bool Enumerate(std::array<SdlDeviceId, MaxEnumeratedGamepads>& ids, std::size_t& count) noexcept;
    bool EnumerateJoysticks(std::array<SdlDeviceId, MaxEnumeratedGamepads>& ids, std::size_t& count) noexcept;
    bool IsGamepad(SdlDeviceId id) noexcept;
    const char* GetJoystickNameForId(SdlDeviceId id) noexcept;
    const char* GetJoystickPathForId(SdlDeviceId id) noexcept;
    std::uint16_t GetJoystickVendorForId(SdlDeviceId id) noexcept;
    std::uint16_t GetJoystickProductForId(SdlDeviceId id) noexcept;
    bool GetJoystickGuidForId(SdlDeviceId id, char* output, std::size_t outputCount) noexcept;
    SdlGamepadHandle OpenGamepad(SdlDeviceId id) noexcept;
    void CloseGamepad(SdlGamepadHandle gamepad) noexcept;
    bool GamepadConnected(SdlGamepadHandle gamepad) noexcept;
    void UpdateGamepads() noexcept;
    bool GetGamepadButton(SdlGamepadHandle gamepad, int button) noexcept;
    std::int16_t GetGamepadAxis(SdlGamepadHandle gamepad, int axis) noexcept;
    bool RumbleGamepad(SdlGamepadHandle gamepad, std::uint16_t low, std::uint16_t high, std::uint32_t durationMs) noexcept;
    bool CanRumble() const noexcept { return sdlRumble_ != nullptr; }

    std::uint16_t GetGamepadVendor(SdlGamepadHandle gamepad) noexcept;
    std::uint16_t GetGamepadProduct(SdlGamepadHandle gamepad) noexcept;
    std::uint16_t GetGamepadProductVersion(SdlGamepadHandle gamepad) noexcept;
    std::uint16_t GetGamepadFirmwareVersion(SdlGamepadHandle gamepad) noexcept;
    int GetGamepadType(SdlGamepadHandle gamepad) noexcept;
    const char* GetGamepadSerial(SdlGamepadHandle gamepad) noexcept;
    bool GetGamepadGuidForId(SdlDeviceId id, char* output, std::size_t outputCount) noexcept;
    ConnectionKind GetConnectionKind(SdlGamepadHandle gamepad) noexcept;
    const char* GetGamepadName(SdlGamepadHandle gamepad) noexcept;
    const char* GetGamepadPath(SdlGamepadHandle gamepad) noexcept;
    char* GetGamepadMapping(SdlGamepadHandle gamepad) noexcept;
    SdlJoystickHandle GetGamepadJoystick(SdlGamepadHandle gamepad) noexcept;
    int GetNumJoystickAxes(SdlJoystickHandle joystick) noexcept;
    std::int16_t GetJoystickAxis(SdlJoystickHandle joystick, int axis) noexcept;
    int GetNumJoystickButtons(SdlJoystickHandle joystick) noexcept;
    int GetNumJoystickHats(SdlJoystickHandle joystick) noexcept;
    bool GetJoystickButton(SdlJoystickHandle joystick, int button) noexcept;

    bool CanAddGamepadMappings() const noexcept { return sdlAddMappings_ != nullptr; }
    int AddGamepadMappingsFromFile(const char* path) noexcept;
    const char* GetError() noexcept;
    void FreeMemory(void* memory) noexcept;

private:
    bool Resolve(HMODULE module) noexcept;

    HMODULE module_{};
    bool initialized_{};
    wchar_t versionText_[32]{L"未知"};

    SDL_SetHintFn sdlSetHint_{};
    SDL_SetHintWithPriorityFn sdlSetHintWithPriority_{};
    SDL_GetHintFn sdlGetHint_{};
    SDL_InitSubSystemFn sdlInit_{};
    SDL_QuitSubSystemFn sdlQuit_{};
    SDL_GetGamepadsFn sdlGetGamepads_{};
    SDL_GetJoysticksFn sdlGetJoysticks_{};
    SDL_IsGamepadFn sdlIsGamepad_{};
    SDL_GetJoystickNameForIDFn sdlJoystickNameForId_{};
    SDL_GetJoystickPathForIDFn sdlJoystickPathForId_{};
    SDL_GetJoystickVendorForIDFn sdlJoystickVendorForId_{};
    SDL_GetJoystickProductForIDFn sdlJoystickProductForId_{};
    SDL_GetJoystickGUIDForIDFn sdlJoystickGuidForId_{};
    SDL_GetGamepadGUIDForIDFn sdlGamepadGuidForId_{};
    SDL_GUIDToStringFn sdlGuidToString_{};
    SDL_OpenGamepadFn sdlOpen_{};
    SDL_CloseGamepadFn sdlClose_{};
    SDL_GamepadConnectedFn sdlConnected_{};
    SDL_SetGamepadEventsEnabledFn sdlSetEventsEnabled_{};
    SDL_SetJoystickEventsEnabledFn sdlSetJoystickEventsEnabled_{};
    SDL_UpdateGamepadsFn sdlUpdate_{};
    SDL_GetGamepadButtonFn sdlButton_{};
    SDL_GetGamepadAxisFn sdlAxis_{};
    SDL_RumbleGamepadFn sdlRumble_{};
    SDL_FreeFn sdlFree_{};
    SDL_GetGamepadVendorFn sdlVendor_{};
    SDL_GetGamepadProductFn sdlProduct_{};
    SDL_GetGamepadProductVersionFn sdlProductVersion_{};
    SDL_GetGamepadFirmwareVersionFn sdlFirmwareVersion_{};
    SDL_GetGamepadSerialFn sdlSerial_{};
    SDL_GetGamepadTypeFn sdlType_{};
    SDL_GetGamepadConnectionStateFn sdlConnection_{};
    SDL_AddGamepadMappingsFromFileFn sdlAddMappings_{};
    SDL_GetErrorFn sdlError_{};
    SDL_GetVersionFn sdlVersion_{};
    SDL_GetGamepadNameFn sdlName_{};
    SDL_GetGamepadPathFn sdlPath_{};
    SDL_GetGamepadMappingFn sdlMapping_{};
    SDL_GetGamepadJoystickFn sdlJoystick_{};
    SDL_GetNumJoystickAxesFn sdlNumAxes_{};
    SDL_GetJoystickAxisFn sdlRawAxis_{};
    SDL_GetNumJoystickButtonsFn sdlNumButtons_{};
    SDL_GetJoystickButtonFn sdlRawButton_{};
    SDL_GetNumJoystickHatsFn sdlNumHats_{};
};

}  // namespace joyshim
