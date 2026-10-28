#include <windows.h>

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdarg>
#include <cwchar>
#include <cstdio>
#include <cstring>
#include <string>

#include "config.hpp"
#include "mapping_database.hpp"
#include "sdl_backend.hpp"

namespace {

using joyshim::SdlBackend;
using joyshim::SdlDeviceId;

const char* SafeText(const char* value) noexcept {
    return value != nullptr && value[0] != '\0' ? value : "(不可用)";
}

const wchar_t* ConnectionName(joyshim::ConnectionKind connection) noexcept {
    switch (connection) {
    case joyshim::ConnectionKind::Wired: return L"Wired";
    case joyshim::ConnectionKind::Wireless: return L"Wireless";
    default: return L"Unknown";
    }
}

const wchar_t* GamepadTypeName(int type) noexcept {
    switch (type) {
    case SDL_GAMEPAD_TYPE_STANDARD: return L"Standard";
    case SDL_GAMEPAD_TYPE_XBOX360: return L"Xbox 360";
    case SDL_GAMEPAD_TYPE_XBOXONE: return L"Xbox One";
    case SDL_GAMEPAD_TYPE_PS3: return L"PlayStation 3";
    case SDL_GAMEPAD_TYPE_PS4: return L"PlayStation 4";
    case SDL_GAMEPAD_TYPE_PS5: return L"PlayStation 5";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return L"Nintendo Switch Pro";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: return L"Switch Joy-Con Left";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return L"Switch Joy-Con Right";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return L"Switch Joy-Con Pair";
    case SDL_GAMEPAD_TYPE_GAMECUBE: return L"GameCube";
    case SDL_GAMEPAD_TYPE_STEAM: return L"Steam Controller";
    default: return L"Unknown";
    }
}

std::string EscapeToml(const char* source) {
    std::string output;
    if (source == nullptr) return output;
    for (const unsigned char c : std::string(source)) {
        switch (c) {
        case '\\': output += "\\\\"; break;
        case '"': output += "\\\""; break;
        case '\b': output += "\\b"; break;
        case '\t': output += "\\t"; break;
        case '\n': output += "\\n"; break;
        case '\f': output += "\\f"; break;
        case '\r': output += "\\r"; break;
        default:
            if (c < 0x20) {
                char escaped[8]{};
                std::snprintf(escaped, sizeof(escaped), "\\u%04X", static_cast<unsigned int>(c));
                output += escaped;
            } else {
                output.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return output;
}

std::string MakeAlias(const char* name, std::size_t index) {
    std::string alias;
    if (name != nullptr) {
        bool uppercaseNext = true;
        for (const unsigned char c : std::string(name)) {
            if (std::isalnum(c) != 0 && c < 0x80) {
                char value = static_cast<char>(c);
                if (uppercaseNext && value >= 'a' && value <= 'z') {
                    value = static_cast<char>(value - ('a' - 'A'));
                }
                alias.push_back(value);
                uppercaseNext = false;
            } else {
                uppercaseNext = true;
            }
            if (alias.size() >= 40) break;
        }
    }
    if (alias.empty() || (alias[0] >= '0' && alias[0] <= '9')) {
        alias = "Controller";
    }
    alias += std::to_string(index + 1);
    return alias;
}

bool SameIds(
    const std::array<SdlDeviceId, joyshim::MaxEnumeratedGamepads>& left,
    std::size_t leftCount,
    const std::array<SdlDeviceId, joyshim::MaxEnumeratedGamepads>& right,
    std::size_t rightCount) noexcept {
    if (leftCount != rightCount) return false;
    for (std::size_t index = 0; index < leftCount; ++index) {
        if (left[index] != right[index]) return false;
    }
    return true;
}

void AppendUtf8(std::string& output, const wchar_t* text) {
    if (text == nullptr) return;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return;
    const std::size_t offset = output.size();
    output.resize(offset + static_cast<std::size_t>(bytes));
    WideCharToMultiByte(
        CP_UTF8,
        0,
        text,
        -1,
        output.data() + offset,
        bytes,
        nullptr,
        nullptr);
    output.resize(offset + static_cast<std::size_t>(bytes - 1));
}

void AppendFormat(std::string& output, const char* format, ...) {
    char buffer[8192]{};
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    if (written <= 0) return;
    const std::size_t count = static_cast<std::size_t>(written) < sizeof(buffer)
        ? static_cast<std::size_t>(written) : sizeof(buffer) - 1;
    output.append(buffer, count);
}

bool WriteReport(const std::string& report) noexcept {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    wchar_t* separator = wcsrchr(path, L'\\');
    if (separator == nullptr) return false;
    *(separator + 1) = L'\0';
    if (wcslen(path) + wcslen(L"JoyShim_GamepadInfo.txt") >= MAX_PATH) return false;
    const wchar_t* reportName = L"JoyShim_GamepadInfo.txt";
    const std::size_t baseLength = std::wcslen(path);
    const std::size_t nameLength = std::wcslen(reportName);
    std::wmemcpy(path + baseLength, reportName, nameLength + 1);

    HANDLE file = CreateFileW(
        path,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    static constexpr unsigned char Bom[] = {0xEF, 0xBB, 0xBF};
    DWORD ignored = 0;
    WriteFile(file, Bom, static_cast<DWORD>(sizeof(Bom)), &ignored, nullptr);
    const bool sizeOkay = report.size() <= 0xFFFFFFFFu;
    const BOOL wrote = sizeOkay
        ? WriteFile(file, report.data(), static_cast<DWORD>(report.size()), &ignored, nullptr)
        : FALSE;
    CloseHandle(file);
    return wrote == TRUE;
}

std::string BuildReport(
    SdlBackend& backend,
    const std::array<SdlDeviceId, joyshim::MaxEnumeratedGamepads>& ids,
    std::size_t count) {
    std::string report;
    AppendFormat(report, "JoyShim Gamepad Info v0.5.2-test12\r\n");
    report += "SDL3 版本=";
    AppendUtf8(report, backend.VersionText());
    AppendFormat(report, "\r\n检测时间=%llu ms\r\n设备数=%u\r\n\r\n",
        static_cast<unsigned long long>(GetTickCount64()),
        static_cast<unsigned int>(count));

    for (std::size_t index = 0; index < count; ++index) {
        const SdlDeviceId id = ids[index];
        const bool isGamepad = backend.IsGamepad(id);
        char guid[33]{};
        backend.GetJoystickGuidForId(id, guid, sizeof(guid));
        const char* joystickName = backend.GetJoystickNameForId(id);
        const char* joystickPath = backend.GetJoystickPathForId(id);
        const std::uint16_t joystickVendor = backend.GetJoystickVendorForId(id);
        const std::uint16_t joystickProduct = backend.GetJoystickProductForId(id);

        AppendFormat(report,
            "============================================================\r\n"
            "设备 %u\r\n"
            "SDL Joystick ID=%u\r\n"
            "Gamepad=%s\r\n"
            "GUID=%s\r\n"
            "名称=%s\r\n"
            "路径=%s\r\n"
            "VID:PID=%04X:%04X\r\n",
            static_cast<unsigned int>(index + 1),
            static_cast<unsigned int>(id),
            isGamepad ? "是" : "否",
            guid[0] != '\0' ? guid : "(不可用)",
            SafeText(joystickName),
            SafeText(joystickPath),
            static_cast<unsigned int>(joystickVendor),
            static_cast<unsigned int>(joystickProduct));

        joyshim::SdlGamepadHandle gamepad = nullptr;
        if (isGamepad) gamepad = backend.OpenGamepad(id);
        if (gamepad != nullptr) {
            const char* name = backend.GetGamepadName(gamepad);
            const char* path = backend.GetGamepadPath(gamepad);
            const char* serial = backend.GetGamepadSerial(gamepad);
            const std::uint16_t vendor = backend.GetGamepadVendor(gamepad);
            const std::uint16_t product = backend.GetGamepadProduct(gamepad);
            const std::uint16_t productVersion = backend.GetGamepadProductVersion(gamepad);
            const std::uint16_t firmwareVersion = backend.GetGamepadFirmwareVersion(gamepad);
            const auto connection = backend.GetConnectionKind(gamepad);
            const auto joystick = backend.GetGamepadJoystick(gamepad);
            const int axes = joystick != nullptr ? backend.GetNumJoystickAxes(joystick) : 0;
            const int buttons = joystick != nullptr ? backend.GetNumJoystickButtons(joystick) : 0;
            const int hats = joystick != nullptr ? backend.GetNumJoystickHats(joystick) : 0;
            char gamepadGuid[33]{};
            backend.GetGamepadGuidForId(id, gamepadGuid, sizeof(gamepadGuid));
            char* mapping = backend.GetGamepadMapping(gamepad);

            std::string connectionUtf8;
            AppendUtf8(connectionUtf8, ConnectionName(connection));
            std::string typeUtf8;
            AppendUtf8(typeUtf8, GamepadTypeName(backend.GetGamepadType(gamepad)));
            AppendFormat(report,
                "打开后的 Gamepad 信息：\r\n"
                "  GUID=%s\r\n"
                "  名称=%s\r\n"
                "  路径=%s\r\n"
                "  序列号=%s\r\n"
                "  VID:PID=%04X:%04X\r\n"
                "  产品版本=%u\r\n"
                "  固件版本=%u\r\n"
                "  类型=%s\r\n"
                "  连接方式=%s\r\n"
                "  原始轴=%d，原始按键=%d，Hat=%d\r\n"
                "  SDL Mapping=%s\r\n",
                gamepadGuid[0] != '\0' ? gamepadGuid : "(不可用)",
                SafeText(name),
                SafeText(path),
                SafeText(serial),
                static_cast<unsigned int>(vendor),
                static_cast<unsigned int>(product),
                static_cast<unsigned int>(productVersion),
                static_cast<unsigned int>(firmwareVersion),
                typeUtf8.c_str(),
                connectionUtf8.c_str(),
                axes,
                buttons,
                hats,
                SafeText(mapping));

            const std::string alias = MakeAlias(name != nullptr ? name : joystickName, index);
            const std::string escapedName = EscapeToml(name != nullptr ? name : joystickName);
            const std::string escapedSerial = EscapeToml(serial);
            AppendFormat(report,
                "\r\n可复制到代理同名 TOML 的手柄子集合：\r\n\r\n"
                "[Controllers.%s]\r\n"
                "Enabled = true\r\n"
                "Priority = 100\r\n",
                alias.c_str());
            if (vendor != 0 || product != 0) {
                AppendFormat(report,
                    "DeviceIds = [\"%04X:%04X\"]\r\n",
                    static_cast<unsigned int>(vendor),
                    static_cast<unsigned int>(product));
            }
            AppendFormat(report,
                "Names = [\"%s\"]\r\n",
                escapedName.c_str());
            if (serial != nullptr && serial[0] != '\0') {
                AppendFormat(report,
                    "# Serials = [\"%s\"]  # 只在需要区分同型号实体时取消注释\r\n",
                    escapedSerial.c_str());
            } else {
                report += "# Serials = [\"\"]  # 设备未提供序列号\r\n";
            }
            AppendFormat(report,
                "# NameContains = [\"%s\"]\r\n"
                "# Connections = [\"%s\"]\r\n",
                escapedName.c_str(),
                connectionUtf8.c_str());
            const char* preferredGuid = gamepadGuid[0] != '\0' ? gamepadGuid : guid;
            if (preferredGuid != nullptr && preferredGuid[0] != '\0') {
                AppendFormat(report,
                    "# Guids = [\"%s\"]  # 永远作为最后兜底；可填写多个\r\n",
                    preferredGuid);
            }
            AppendFormat(report,
                "\r\n"
                "# 下面的设置只作用于 Controllers.%s；未填写项继承主项目设置。\r\n"
                "# [Controllers.%s.NativeInput]\r\n"
                "# HideDirectInput = false\r\n"
                "#\r\n"
                "# [Controllers.%s.Axes]\r\n"
                "# RightStickY = \"Gamepad.RightY.Inverted\"\r\n"
                "\r\n",
                alias.c_str(),
                alias.c_str(),
                alias.c_str());

            if (mapping != nullptr) backend.FreeMemory(mapping);
            backend.CloseGamepad(gamepad);
        } else if (isGamepad) {
            report += "错误：SDL 将设备标记为 Gamepad，但 SDL_OpenGamepad 失败。\r\n\r\n";
        } else {
            report += "说明：该设备仅进入 SDL Joystick 层，不能直接生成 Gamepad Profile。\r\n\r\n";
        }
    }
    if (count == 0) {
        report += "当前没有 SDL Joystick。连接或重新插入手柄后，本工具会自动刷新。\r\n";
    }
    return report;
}

}  // namespace

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    std::wprintf(
        L"JoyShim Gamepad Info v0.5.2-test12\n"
        L"连接、拔插或切换手柄后会自动刷新。按 Ctrl+C 退出。\n\n");

    const joyshim::RuntimeConfig config =
        joyshim::LoadRuntimeConfig(GetModuleHandleW(nullptr));
    SdlBackend backend;
    wchar_t detail[512]{};
    if (!backend.Initialize(config, detail, std::size(detail))) {
        std::wprintf(L"SDL3 初始化失败：%ls\n", detail);
        return 1;
    }
    std::wprintf(L"%ls\n", detail);
    const joyshim::MappingLoadResult mapping =
        joyshim::LoadConfiguredMappingDatabase(config, backend);
    if (mapping.requested) std::wprintf(L"%ls\n", mapping.detail);

    std::array<SdlDeviceId, joyshim::MaxEnumeratedGamepads> previous{};
    std::size_t previousCount = 0;
    bool initialized = false;
    for (;;) {
        backend.UpdateGamepads();
        std::array<SdlDeviceId, joyshim::MaxEnumeratedGamepads> ids{};
        std::size_t count = 0;
        if (backend.EnumerateJoysticks(ids, count) &&
            (!initialized || !SameIds(previous, previousCount, ids, count))) {
            previous = ids;
            previousCount = count;
            initialized = true;
            const std::string report = BuildReport(backend, ids, count);
            const bool wrote = WriteReport(report);
            std::printf("\n%s\n", report.c_str());
            std::wprintf(
                wrote
                    ? L"报告已更新：JoyShim_GamepadInfo.txt\n"
                    : L"警告：无法写入 JoyShim_GamepadInfo.txt\n");
        }
        Sleep(250);
    }
}
