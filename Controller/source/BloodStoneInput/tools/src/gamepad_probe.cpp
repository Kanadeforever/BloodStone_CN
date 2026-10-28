#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cwchar>

#include "config.hpp"
#include "device_correction.hpp"
#include "input_routing.hpp"
#include "mapping_database.hpp"
#include "sdl_backend.hpp"

namespace {

struct ProbeReader {
    joyshim::SdlBackend& backend;
    joyshim::SdlGamepadHandle gamepad;
    joyshim::SdlJoystickHandle joystick;
    const joyshim::DeviceProfile& profile;
    int configuredRawAxisCount;
    int configuredRawButtonCount;
    joyshim::StandardAxisValues standard{};
    std::array<bool, static_cast<std::size_t>(
        joyshim::StandardButton::Count)> standardButtons{};
    std::array<std::int16_t, 256> rawAxes{};
    std::array<bool, 256> rawButtons{};
    std::size_t rawAxisCount{};
    std::size_t rawButtonCount{};

    ProbeReader(
        joyshim::SdlBackend& selectedBackend,
        joyshim::SdlGamepadHandle selectedGamepad,
        joyshim::SdlJoystickHandle selectedJoystick,
        const joyshim::DeviceProfile& selectedProfile,
        int selectedRawAxisCount,
        int selectedRawButtonCount) noexcept
        : backend(selectedBackend),
          gamepad(selectedGamepad),
          joystick(selectedJoystick),
          profile(selectedProfile),
          configuredRawAxisCount(selectedRawAxisCount),
          configuredRawButtonCount(selectedRawButtonCount) {
        Capture();
    }

    void Capture() noexcept {
        for (std::size_t index = 0; index < standardButtons.size(); ++index) {
            standardButtons[index] = backend.GetGamepadButton(
                gamepad, static_cast<int>(index));
        }

        const joyshim::AxisPair left = joyshim::ApplyAxisCorrection(
            backend.GetGamepadAxis(gamepad, 0),
            backend.GetGamepadAxis(gamepad, 1),
            profile.correction.leftStick);
        const joyshim::AxisPair right = joyshim::ApplyAxisCorrection(
            backend.GetGamepadAxis(gamepad, 2),
            backend.GetGamepadAxis(gamepad, 3),
            profile.correction.rightStick);
        standard.leftX = left.x;
        standard.leftY = left.y;
        standard.rightX = right.x;
        standard.rightY = right.y;
        standard.leftTrigger = backend.GetGamepadAxis(gamepad, 4);
        standard.rightTrigger = backend.GetGamepadAxis(gamepad, 5);

        const int boundedAxisCount = configuredRawAxisCount < 0 ? 0 :
            (configuredRawAxisCount > static_cast<int>(rawAxes.size())
                ? static_cast<int>(rawAxes.size()) : configuredRawAxisCount);
        rawAxisCount = static_cast<std::size_t>(boundedAxisCount);
        for (std::size_t index = 0; index < rawAxisCount; ++index) {
            rawAxes[index] = backend.GetJoystickAxis(
                joystick, static_cast<int>(index));
        }

        const int boundedButtonCount = configuredRawButtonCount < 0 ? 0 :
            (configuredRawButtonCount > static_cast<int>(rawButtons.size())
                ? static_cast<int>(rawButtons.size()) : configuredRawButtonCount);
        rawButtonCount = static_cast<std::size_t>(boundedButtonCount);
        for (std::size_t index = 0; index < rawButtonCount; ++index) {
            rawButtons[index] = backend.GetJoystickButton(
                joystick, static_cast<int>(index));
        }
    }

    bool Button(const joyshim::ButtonSource& source) const noexcept {
        if (source.kind == joyshim::ButtonSourceKind::Standard) {
            return source.index < standardButtons.size() &&
                standardButtons[source.index];
        }
        if (source.kind == joyshim::ButtonSourceKind::Raw) {
            return source.index < rawButtonCount && rawButtons[source.index];
        }
        return false;
    }

    std::int16_t Axis(const joyshim::HalfAxisSource& source) const noexcept {
        if (source.kind == joyshim::AxisSourceKind::Standard) {
            if (source.index >= static_cast<std::uint16_t>(
                joyshim::StandardAxis::Count)) return 0;
            return standard.Get(
                static_cast<joyshim::StandardAxis>(source.index));
        }
        if (source.kind == joyshim::AxisSourceKind::Raw) {
            return source.index < rawAxisCount ? rawAxes[source.index] : 0;
        }
        return 0;
    }

    std::int16_t RawAxis(int axis) const noexcept {
        return axis >= 0 && static_cast<std::size_t>(axis) < rawAxisCount
            ? rawAxes[static_cast<std::size_t>(axis)] : 0;
    }
};

bool SameIds(
    const std::array<joyshim::SdlDeviceId,
        joyshim::MaxEnumeratedGamepads>& left,
    std::size_t leftCount,
    const std::array<joyshim::SdlDeviceId,
        joyshim::MaxEnumeratedGamepads>& right,
    std::size_t rightCount) noexcept {
    if (leftCount != rightCount) return false;
    for (std::size_t index = 0; index < leftCount; ++index) {
        if (left[index] != right[index]) return false;
    }
    return true;
}

void PrintJoystickDiscovery(
    joyshim::SdlBackend& backend,
    std::array<joyshim::SdlDeviceId,
        joyshim::MaxEnumeratedGamepads>& previousIds,
    std::size_t& previousCount,
    bool& initialized) noexcept {
    std::array<joyshim::SdlDeviceId,
        joyshim::MaxEnumeratedGamepads> ids{};
    std::size_t count = 0;
    if (!backend.EnumerateJoysticks(ids, count)) return;
    if (initialized && SameIds(previousIds, previousCount, ids, count)) return;

    previousIds = ids;
    previousCount = count;
    initialized = true;
    std::wprintf(L"\nSDL3 原始 Joystick 列表变化：当前设备数=%u\n",
        static_cast<unsigned int>(count));
    for (std::size_t index = 0; index < count; ++index) {
        char guid[33]{};
        backend.GetJoystickGuidForId(ids[index], guid, sizeof(guid));
        std::wprintf(
            L"  ID=%u Gamepad=%ls GUID=%hs VID:PID=%04X:%04X 名称=%hs 路径=%hs\n",
            static_cast<unsigned int>(ids[index]),
            backend.IsGamepad(ids[index]) ? L"是" : L"否",
            guid[0] != '\0' ? guid : "(unknown)",
            static_cast<unsigned int>(backend.GetJoystickVendorForId(ids[index])),
            static_cast<unsigned int>(backend.GetJoystickProductForId(ids[index])),
            backend.GetJoystickNameForId(ids[index]) != nullptr
                ? backend.GetJoystickNameForId(ids[index]) : "(unknown)",
            backend.GetJoystickPathForId(ids[index]) != nullptr
                ? backend.GetJoystickPathForId(ids[index]) : "(unknown)");
    }
}

}  // namespace

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    const joyshim::RuntimeConfig config =
        joyshim::LoadRuntimeConfig(GetModuleHandleW(nullptr));

    joyshim::SdlBackend backend;
    wchar_t detail[512]{};
    std::wprintf(L"固定后端：SDL3\n");
    if (!backend.Initialize(config, detail, 512)) {
        std::wprintf(L"初始化失败：%ls\n", detail);
        return 1;
    }
    std::wprintf(L"%ls\n", detail);
    const joyshim::MappingLoadResult mapping =
        joyshim::LoadConfiguredMappingDatabase(config, backend);
    if (mapping.requested) std::wprintf(L"%ls\n", mapping.detail);

    joyshim::SdlGamepadHandle gamepad = nullptr;
    joyshim::SdlDeviceId activeId = 0;
    joyshim::SdlDeviceId reportedProfileId = 0;
    std::array<joyshim::SdlDeviceId,
        joyshim::MaxEnumeratedGamepads> discoveredIds{};
    std::size_t discoveredCount = 0;
    bool discoveryInitialized = false;
    for (;;) {
        backend.UpdateGamepads();
        PrintJoystickDiscovery(
            backend, discoveredIds, discoveredCount, discoveryInitialized);
        if (gamepad != nullptr && !backend.GamepadConnected(gamepad)) {
            backend.CloseGamepad(gamepad);
            gamepad = nullptr;
            activeId = 0;
            reportedProfileId = 0;
            std::wprintf(L"\n手柄已断开，继续等待。\n");
        }
        if (gamepad == nullptr) {
            std::array<joyshim::SdlDeviceId, joyshim::MaxEnumeratedGamepads> ids{};
            std::size_t count = 0;
            if (backend.Enumerate(ids, count) && count != 0) {
                gamepad = backend.OpenGamepad(ids[0]);
                activeId = ids[0];
                if (gamepad != nullptr) {
                    std::wprintf(
                        L"已打开手柄：后端=%ls，ID=%u，VID:PID=%04X:%04X，名称=%hs\n",
                        backend.Name(),
                        static_cast<unsigned int>(activeId),
                        static_cast<unsigned int>(backend.GetGamepadVendor(gamepad)),
                        static_cast<unsigned int>(backend.GetGamepadProduct(gamepad)),
                        backend.GetGamepadName(gamepad) != nullptr
                            ? backend.GetGamepadName(gamepad) : "(unknown)");
                }
            }
        }
        if (gamepad == nullptr) {
            std::wprintf(L"\r未检测到 Gamepad，按 Ctrl+C 退出。                    ");
            Sleep(250);
            continue;
        }

        joyshim::DeviceIdentity identity{};
        identity.vendor = backend.GetGamepadVendor(gamepad);
        identity.product = backend.GetGamepadProduct(gamepad);
        identity.connection = backend.GetConnectionKind(gamepad);
        backend.GetGamepadGuidForId(activeId, identity.guid, sizeof(identity.guid));
        const char* name = backend.GetGamepadName(gamepad);
        const char* serial = backend.GetGamepadSerial(gamepad);
        const char* path = backend.GetGamepadPath(gamepad);
        if (name != nullptr) std::snprintf(identity.name, sizeof(identity.name), "%s", name);
        if (serial != nullptr) std::snprintf(identity.serial, sizeof(identity.serial), "%s", serial);
        if (path != nullptr) std::snprintf(identity.path, sizeof(identity.path), "%s", path);
        const joyshim::DeviceProfile profile =
            joyshim::LoadDeviceProfile(config, identity);
        if (reportedProfileId != activeId) {
            const auto& tuning = profile.outputTuning.rightStick.diagonal;
            const wchar_t* biasAxis = tuning.axis == joyshim::DiagonalBiasAxis::X
                ? L"X" : L"Y";
            const wchar_t* signMode = L"All";
            if (tuning.signMode == joyshim::DiagonalSignMode::SameSigns) {
                signMode = L"SameSigns";
            } else if (tuning.signMode == joyshim::DiagonalSignMode::OppositeSigns) {
                signMode = L"OppositeSigns";
            }
            const wchar_t* separationMode = L"None";
            if (profile.outputTuning.rightStick.separation.mode == joyshim::AxisSeparationMode::PreferX) {
                separationMode = L"PreferX";
            } else if (profile.outputTuning.rightStick.separation.mode == joyshim::AxisSeparationMode::PreferY) {
                separationMode = L"PreferY";
            } else if (profile.outputTuning.rightStick.separation.mode == joyshim::AxisSeparationMode::DominantAxis) {
                separationMode = L"DominantAxis";
            }
            std::wprintf(
                L"\nProfile：手柄子集合=%hs，主项目设置=%ls，手柄覆盖=%ls\n"
                L"右摇杆：对角线=%ls，容差=%d，旧偏移=%d，最小轴幅=%d，最小主轴差=%d，主轴=%ls，范围=%ls\n"
                L"轴分离：模式=%ls，最小轴幅=%d，副轴比例=%d%%\n",
                profile.controllerAlias[0] != '\0' ? profile.controllerAlias : "(无)",
                profile.projectPatchApplied ? L"已应用" : L"默认",
                profile.controllerPatchApplied ? L"已覆盖" : L"未覆盖",
                tuning.enabled ? L"启用" : L"关闭",
                tuning.tolerance,
                tuning.offset,
                tuning.minimumMagnitude,
                tuning.minimumGap,
                biasAxis,
                signMode,
                separationMode,
                profile.outputTuning.rightStick.separation.minimumMagnitude,
                profile.outputTuning.rightStick.separation.secondaryPercent);
            reportedProfileId = activeId;
        }
        const auto joystick = backend.GetGamepadJoystick(gamepad);
        const int rawAxisCount = joystick != nullptr ? backend.GetNumJoystickAxes(joystick) : 0;
        const int rawButtonCount = joystick != nullptr ? backend.GetNumJoystickButtons(joystick) : 0;

        ProbeReader reader{backend, gamepad, joystick, profile, rawAxisCount, rawButtonCount};
        const joyshim::RoutedState routed = joyshim::EvaluateInputRouting(
            profile.routing,
            [&](const joyshim::ButtonSource& source) noexcept { return reader.Button(source); },
            [&](const joyshim::HalfAxisSource& source) noexcept { return reader.Axis(source); });
        const joyshim::AxisPair correctedRight = joyshim::ApplyAxisCorrection(
            routed.rightX, routed.rightY, profile.outputCorrection.rightStick);
        const joyshim::AxisPair outputRight = joyshim::ApplyAxisOutputTuning(
            correctedRight.x, correctedRight.y, profile.outputTuning.rightStick);

        std::wprintf(
            L"\r%ls Std RX=%6d RY=%6d | Route RX=%6d RY=%6d | Final RX=%6d RY=%6d | Raw:",
            backend.Name(),
            static_cast<int>(reader.standard.rightX),
            static_cast<int>(reader.standard.rightY),
            static_cast<int>(routed.rightX),
            static_cast<int>(routed.rightY),
            static_cast<int>(outputRight.x),
            static_cast<int>(outputRight.y));
        const int shown = rawAxisCount < 8 ? rawAxisCount : 8;
        for (int axis = 0; axis < shown; ++axis) {
            std::wprintf(L" a%d=%6d", axis, static_cast<int>(reader.RawAxis(axis)));
        }
        std::wprintf(L"       ");
        std::fflush(stdout);
        Sleep(16);
    }
}
