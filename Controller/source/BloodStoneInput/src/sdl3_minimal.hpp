#pragma once

#include <cstdint>

// 静态构建使用官方 SDL3 头文件，并将官方静态库链接进各代理 DLL。
// 非正式动态诊断路径才使用下方的最小 C ABI 声明与运行时函数解析。
#if defined(JOYSHIM_SDL_STATIC)
// 静态链接时使用 SDL 官方声明，避免手写 ABI 与新版 SDL3 头文件不一致。
#include <SDL3/SDL.h>
#else
struct SDL_Gamepad;
struct SDL_Joystick;
using SDL_JoystickID = std::uint32_t;

struct SDL_GUID {
    std::uint8_t data[16]{};
};

constexpr std::uint32_t SDL_INIT_GAMEPAD = 0x00002000u;

enum SDL_GamepadAxis : int {
    SDL_GAMEPAD_AXIS_INVALID = -1,
    SDL_GAMEPAD_AXIS_LEFTX = 0,
    SDL_GAMEPAD_AXIS_LEFTY,
    SDL_GAMEPAD_AXIS_RIGHTX,
    SDL_GAMEPAD_AXIS_RIGHTY,
    SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
    SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
    SDL_GAMEPAD_AXIS_COUNT,
};

enum SDL_GamepadButton : int {
    SDL_GAMEPAD_BUTTON_INVALID = -1,
    SDL_GAMEPAD_BUTTON_SOUTH = 0,
    SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_WEST,
    SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_BACK,
    SDL_GAMEPAD_BUTTON_GUIDE,
    SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_BUTTON_DPAD_UP,
    SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
    SDL_GAMEPAD_BUTTON_MISC1,
    SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,
    SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,
    SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2,
    SDL_GAMEPAD_BUTTON_LEFT_PADDLE2,
    SDL_GAMEPAD_BUTTON_TOUCHPAD,
    SDL_GAMEPAD_BUTTON_MISC2,
    SDL_GAMEPAD_BUTTON_MISC3,
    SDL_GAMEPAD_BUTTON_MISC4,
    SDL_GAMEPAD_BUTTON_MISC5,
    SDL_GAMEPAD_BUTTON_MISC6,
    SDL_GAMEPAD_BUTTON_COUNT,
};

enum SDL_GamepadType : int {
    SDL_GAMEPAD_TYPE_UNKNOWN = 0,
    SDL_GAMEPAD_TYPE_STANDARD,
    SDL_GAMEPAD_TYPE_XBOX360,
    SDL_GAMEPAD_TYPE_XBOXONE,
    SDL_GAMEPAD_TYPE_PS3,
    SDL_GAMEPAD_TYPE_PS4,
    SDL_GAMEPAD_TYPE_PS5,
    SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO,
    SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT,
    SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT,
    SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR,
    SDL_GAMEPAD_TYPE_GAMECUBE,
    SDL_GAMEPAD_TYPE_STEAM,
};

enum SDL_JoystickConnectionState : int {
    SDL_JOYSTICK_CONNECTION_INVALID = -1,
    SDL_JOYSTICK_CONNECTION_UNKNOWN,
    SDL_JOYSTICK_CONNECTION_WIRED,
    SDL_JOYSTICK_CONNECTION_WIRELESS,
};

enum SDL_HintPriority : int {
    SDL_HINT_DEFAULT = 0,
    SDL_HINT_NORMAL,
    SDL_HINT_OVERRIDE,
};

#endif

using SDL_SetHintFn = bool(__cdecl*)(const char* name, const char* value);
using SDL_SetHintWithPriorityFn = bool(__cdecl*)(const char* name, const char* value, SDL_HintPriority priority);
using SDL_GetHintFn = const char*(__cdecl*)(const char* name);
using SDL_InitSubSystemFn = bool(__cdecl*)(std::uint32_t flags);
using SDL_QuitSubSystemFn = void(__cdecl*)(std::uint32_t flags);
using SDL_GetGamepadsFn = SDL_JoystickID*(__cdecl*)(int* count);
using SDL_GetJoysticksFn = SDL_JoystickID*(__cdecl*)(int* count);
using SDL_IsGamepadFn = bool(__cdecl*)(SDL_JoystickID instanceId);
using SDL_GetJoystickNameForIDFn = const char*(__cdecl*)(SDL_JoystickID instanceId);
using SDL_GetJoystickPathForIDFn = const char*(__cdecl*)(SDL_JoystickID instanceId);
using SDL_GetJoystickVendorForIDFn = std::uint16_t(__cdecl*)(SDL_JoystickID instanceId);
using SDL_GetJoystickProductForIDFn = std::uint16_t(__cdecl*)(SDL_JoystickID instanceId);
using SDL_GetJoystickGUIDForIDFn = SDL_GUID(__cdecl*)(SDL_JoystickID instanceId);
using SDL_GetGamepadGUIDForIDFn = SDL_GUID(__cdecl*)(SDL_JoystickID instanceId);
using SDL_GUIDToStringFn = void(__cdecl*)(SDL_GUID guid, char* output, int outputSize);
using SDL_OpenGamepadFn = SDL_Gamepad*(__cdecl*)(SDL_JoystickID instanceId);
using SDL_CloseGamepadFn = void(__cdecl*)(SDL_Gamepad* gamepad);
using SDL_GamepadConnectedFn = bool(__cdecl*)(SDL_Gamepad* gamepad);
using SDL_SetGamepadEventsEnabledFn = void(__cdecl*)(bool enabled);
using SDL_SetJoystickEventsEnabledFn = void(__cdecl*)(bool enabled);
using SDL_UpdateGamepadsFn = void(__cdecl*)();
using SDL_GetGamepadButtonFn = bool(__cdecl*)(SDL_Gamepad* gamepad, SDL_GamepadButton button);
using SDL_GetGamepadAxisFn = std::int16_t(__cdecl*)(SDL_Gamepad* gamepad, SDL_GamepadAxis axis);
using SDL_RumbleGamepadFn = bool(__cdecl*)(SDL_Gamepad* gamepad, std::uint16_t low, std::uint16_t high, std::uint32_t durationMs);
using SDL_FreeFn = void(__cdecl*)(void* memory);
using SDL_AddGamepadMappingsFromFileFn = int(__cdecl*)(const char* file);

// 诊断工具使用的可选元数据接口。
using SDL_GetVersionFn = int(__cdecl*)();
using SDL_GetErrorFn = const char*(__cdecl*)();
using SDL_GetGamepadNameFn = const char*(__cdecl*)(SDL_Gamepad* gamepad);
using SDL_GetGamepadPathFn = const char*(__cdecl*)(SDL_Gamepad* gamepad);
using SDL_GetGamepadVendorFn = std::uint16_t(__cdecl*)(SDL_Gamepad* gamepad);
using SDL_GetGamepadProductFn = std::uint16_t(__cdecl*)(SDL_Gamepad* gamepad);
using SDL_GetGamepadProductVersionFn = std::uint16_t(__cdecl*)(SDL_Gamepad* gamepad);
using SDL_GetGamepadFirmwareVersionFn = std::uint16_t(__cdecl*)(SDL_Gamepad* gamepad);
using SDL_GetGamepadSerialFn = const char*(__cdecl*)(SDL_Gamepad* gamepad);
using SDL_GetGamepadTypeFn = SDL_GamepadType(__cdecl*)(SDL_Gamepad* gamepad);
using SDL_GetGamepadConnectionStateFn = SDL_JoystickConnectionState(__cdecl*)(SDL_Gamepad* gamepad);
using SDL_GetGamepadMappingFn = char*(__cdecl*)(SDL_Gamepad* gamepad);

using SDL_GetGamepadJoystickFn = SDL_Joystick*(__cdecl*)(SDL_Gamepad* gamepad);
using SDL_GetNumJoystickAxesFn = int(__cdecl*)(SDL_Joystick* joystick);
using SDL_GetJoystickAxisFn = std::int16_t(__cdecl*)(SDL_Joystick* joystick, int axis);
using SDL_GetNumJoystickButtonsFn = int(__cdecl*)(SDL_Joystick* joystick);
using SDL_GetJoystickButtonFn = bool(__cdecl*)(SDL_Joystick* joystick, int button);
using SDL_GetNumJoystickHatsFn = int(__cdecl*)(SDL_Joystick* joystick);
