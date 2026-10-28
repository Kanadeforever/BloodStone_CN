#include <windows.h>
#ifdef BSCN_SINGLE_DLL
#include "bond_one_log.hpp"
#include "bond_one_bridge.hpp"
#include "bloodstone_device_family.hpp"
#endif

#ifdef _MSC_VER
// Windows 10 SDK 会将 XInputEnable 标记为 deprecated，但代理 DLL 必须
// 保留该导出以兼容 XInput 1.x ABI。
#pragma warning(disable : 4995)
#endif
#include <xinput.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>

#include "config.hpp"
#include "device_correction.hpp"
#include "input_mapping.hpp"
#include "input_routing.hpp"
#include "mapping_database.hpp"
#include "managed_devices.hpp"
#include "proxy_entry.hpp"
#include "runtime_start.hpp"
#include "xinput_exports.hpp"
#include "sdl_backend.hpp"
#include "slot_assignment.hpp"
#include "win32_proc.hpp"

namespace {

static_assert(sizeof(XINPUT_GAMEPAD) == 12, "Unexpected XINPUT_GAMEPAD ABI");
static_assert(sizeof(XINPUT_STATE) == 16, "Unexpected XINPUT_STATE ABI");
static_assert(sizeof(JoyShimXInputCapabilitiesEx) == 32, "Unexpected XInput capabilities-ex ABI");

constexpr ULONGLONG RescanIntervalMs = 250;
constexpr DWORD InputStartupTimeoutMs = 10000;
constexpr LONG RuntimeNotStarted = 0;
constexpr LONG RuntimeInitializing = 1;
constexpr LONG RuntimeReady = 2;
constexpr LONG RuntimeFailed = 3;
// XInput 震动持续到下次 XInputSetState。SDL3 需要时长，因此使用约 49 天的
//最大 Uint32 时长模拟持续震动；下一次调用会覆盖，零强度会立即停止。
constexpr std::uint32_t ContinuousRumbleMs = 0xFFFFFFFFu;

struct WorkerSlot {
    joyshim::SdlDeviceId id{};
    joyshim::SdlGamepadHandle gamepad{};
    joyshim::SdlJoystickHandle joystick{};
    joyshim::DeviceProfile profile{};
    int rawAxisCount{};
    int rawButtonCount{};
    joyshim::DeviceIdentity identity{};
#ifdef BSCN_SINGLE_DLL
    int sdlType{};
    char sdlTypeName[80]{};
    bool stadia{}; // 使用 SDL 映射名识别，避免蓝牙模式名称和 VID 变化导致漏判。
#endif
    std::uint32_t appliedRumbleGeneration{};
    ULONGLONG nextRightStickTraceTime{};
};

void UpdateManagedInputSnapshot(
    const std::array<WorkerSlot, joyshim::MaxXInputUsers>& slots) noexcept;

struct PublishedSlot {
    bool connected{};
    bool rumbleSupported{};
    bool outputTuningActive{};
    XINPUT_GAMEPAD gamepad{};
    DWORD packetNumber{};
    WORD supportedButtons{};
    joyshim::AxisRouting axisRouting{};
    joyshim::DeviceIdentity identity{};
#ifdef BSCN_SINGLE_DLL
    std::uint32_t deviceId{};
    int sdlType{};
    char sdlTypeName[80]{};
#endif
    char controllerAlias[joyshim::MaxProfileAlias]{};
    bool projectPatchApplied{};
    bool controllerPatchApplied{};
};

struct RumbleCommand {
    std::uint16_t left{};
    std::uint16_t right{};
    std::uint32_t generation{};
};

struct FileStamp {
    FILETIME writeTime{};
    DWORD sizeHigh{};
    DWORD sizeLow{};
    bool exists{};
};

struct JoystickDiscoverySnapshot {
    std::array<joyshim::SdlDeviceId,
        joyshim::MaxEnumeratedGamepads> ids{};
    std::size_t count{};
    bool initialized{};
};

INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;
SRWLOCK g_controlLock = SRWLOCK_INIT;
SRWLOCK g_stateLock = SRWLOCK_INIT;
SRWLOCK g_readTraceLock = SRWLOCK_INIT;
HMODULE g_module{};
joyshim::RuntimeConfig g_config{};
std::uint32_t g_configGeneration{1};
std::uint32_t g_mappingReloadGeneration{1};
std::array<RumbleCommand, joyshim::MaxXInputUsers> g_rumbleCommands{};
std::array<PublishedSlot, joyshim::MaxXInputUsers> g_publishedSlots{};
std::array<ULONGLONG, joyshim::MaxXInputUsers> g_nextReadTraceTime{};
std::array<std::uint64_t, joyshim::MaxXInputUsers> g_getStateCallCount{};
std::array<std::uint64_t, joyshim::MaxXInputUsers> g_getStateExCallCount{};
volatile LONG g_initialized = FALSE;
volatile LONG g_runtimeState = RuntimeNotStarted;
volatile LONG g_xinputEntryReported = FALSE;
volatile LONG g_enabled = TRUE;
HANDLE g_inputStopEvent{};
HANDLE g_inputWakeEvent{};
HANDLE g_inputReadyEvent{};
HANDLE g_hotReloadStopEvent{};

void StartHotReloadWatcher() noexcept;
void LogAxisRouting(
    const joyshim::RuntimeConfig& config,
    const joyshim::AxisRouting& routing,
    const wchar_t* prefix) noexcept;

const wchar_t* ConfigSourceName(joyshim::ConfigSourceKind source) noexcept {
    switch (source) {
        case joyshim::ConfigSourceKind::GeneratedGeneric: return L"自动生成的当前目录默认配置";
        case joyshim::ConfigSourceKind::Generic: return L"当前目录默认配置";
        case joyshim::ConfigSourceKind::ExactProcessProfile: return L"当前 EXE 专版";
        case joyshim::ConfigSourceKind::SecondaryProcessProfile: return L"同游戏其他 EXE 专版";
        default: return L"未知";
    }
}

void Utf8ToWide(const char* source, wchar_t* output, int outputCount) noexcept {
    if (output == nullptr || outputCount <= 0) return;
    output[0] = L'\0';
    if (source == nullptr) return;
    MultiByteToWideChar(CP_UTF8, 0, source, -1, output, outputCount);
}

template <std::size_t N>
void CopyUtf8(char (&output)[N], const char* source) noexcept {
    static_assert(N > 0, "destination must include a terminator");
    output[0] = '\0';
    if (source == nullptr) return;
    const std::size_t length = std::strlen(source);
    const std::size_t copyLength = length < N - 1 ? length : N - 1;
    std::memcpy(output, source, copyLength);
    output[copyLength] = '\0';
}

joyshim::RuntimeConfig CopyRuntimeConfig() noexcept {
    AcquireSRWLockShared(&g_controlLock);
    const joyshim::RuntimeConfig copy = g_config;
    ReleaseSRWLockShared(&g_controlLock);
    return copy;
}

bool ExposeGuideInGetState() noexcept {
    AcquireSRWLockShared(&g_controlLock);
    const bool value = g_config.exposeGuideInGetState;
    ReleaseSRWLockShared(&g_controlLock);
    return value;
}

FileStamp QueryFileStamp(const wchar_t* path) noexcept {
    FileStamp stamp{};
    if (path == nullptr || path[0] == L'\0') return stamp;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) return stamp;
    stamp.exists = true;
    stamp.writeTime = data.ftLastWriteTime;
    stamp.sizeHigh = data.nFileSizeHigh;
    stamp.sizeLow = data.nFileSizeLow;
    return stamp;
}

bool SameFileStamp(const FileStamp& left, const FileStamp& right) noexcept {
    return left.exists == right.exists &&
        left.writeTime.dwLowDateTime == right.writeTime.dwLowDateTime &&
        left.writeTime.dwHighDateTime == right.writeTime.dwHighDateTime &&
        left.sizeHigh == right.sizeHigh && left.sizeLow == right.sizeLow;
}

WORD BuildSupportedButtons(const joyshim::InputRouting& routing) noexcept {
    WORD supported = 0;
    for (std::size_t index = 0; index < joyshim::XInputButtonTargetCount; ++index) {
        if (routing.buttons[index].count != 0) {
            supported |= static_cast<WORD>(joyshim::ButtonMaskForTarget(
                static_cast<joyshim::XInputButtonTarget>(index)));
        }
    }
    return supported;
}

void ClearPublishedSlot(std::size_t index) noexcept {
    if (index >= g_publishedSlots.size()) return;
    AcquireSRWLockExclusive(&g_stateLock);
    PublishedSlot& published = g_publishedSlots[index];
    published.connected = false;
    published.rumbleSupported = false;
    published.outputTuningActive = false;
    std::memset(&published.gamepad, 0, sizeof(published.gamepad));
    published.supportedButtons = 0;
    published.axisRouting = {};
    published.identity = {};
#ifdef BSCN_SINGLE_DLL
    published.deviceId = 0;
    published.sdlType = 0;
    published.sdlTypeName[0] = '\0';
#endif
    published.controllerAlias[0] = '\0';
    published.projectPatchApplied = false;
    published.controllerPatchApplied = false;
    ReleaseSRWLockExclusive(&g_stateLock);
}

void PublishWorkerSlot(
    std::size_t index,
    const WorkerSlot& slot,
    const XINPUT_GAMEPAD& state,
    bool rumbleSupported) noexcept {
    if (index >= g_publishedSlots.size()) return;
    AcquireSRWLockExclusive(&g_stateLock);
    PublishedSlot& published = g_publishedSlots[index];
    if (!published.connected ||
        std::memcmp(&published.gamepad, &state, sizeof(state)) != 0) {
        ++published.packetNumber;
    }
    published.connected = true;
    published.rumbleSupported = rumbleSupported;
    published.outputTuningActive =
        joyshim::HasOutputTuning(slot.profile.outputTuning.leftStick) ||
        joyshim::HasOutputTuning(slot.profile.outputTuning.rightStick);
    published.gamepad = state;
    published.supportedButtons = BuildSupportedButtons(slot.profile.routing);
    published.axisRouting = slot.profile.routing.axes;
    published.identity = slot.identity;
#ifdef BSCN_SINGLE_DLL
    published.deviceId = slot.id;
    published.sdlType = slot.sdlType;
    CopyUtf8(published.sdlTypeName, slot.sdlTypeName);
#endif
    CopyUtf8(published.controllerAlias, slot.profile.controllerAlias);
    published.projectPatchApplied = slot.profile.projectPatchApplied;
    published.controllerPatchApplied = slot.profile.controllerPatchApplied;
    ReleaseSRWLockExclusive(&g_stateLock);
}

void LogAxisRouting(
    const joyshim::RuntimeConfig& config,
    const joyshim::AxisRouting& routing,
    const wchar_t* prefix) noexcept {
    if (!config.enableLog || !config.verboseDeviceInfo) return;
    wchar_t directX[96]{}, directY[96]{};
    wchar_t rxl[96]{}, rxr[96]{}, ryu[96]{}, ryd[96]{}, lt[96]{}, rt[96]{};
    joyshim::FormatSignedAxisSource(routing.rightStickX, directX, 96);
    joyshim::FormatSignedAxisSource(routing.rightStickY, directY, 96);
    joyshim::FormatHalfAxisSource(routing.rightStickLeft, rxl, 96);
    joyshim::FormatHalfAxisSource(routing.rightStickRight, rxr, 96);
    joyshim::FormatHalfAxisSource(routing.rightStickUp, ryu, 96);
    joyshim::FormatHalfAxisSource(routing.rightStickDown, ryd, 96);
    joyshim::FormatHalfAxisSource(routing.leftTrigger, lt, 96);
    joyshim::FormatHalfAxisSource(routing.rightTrigger, rt, 96);
    wchar_t message[1280]{};
    std::swprintf(
        message,
        1280,
        L"%ls轴路由：RX直接=%ls，RY直接=%ls；兼容半轴 RX 左=%ls，右=%ls；RY 上=%ls，下=%ls；LT=%ls；RT=%ls。",
        prefix != nullptr ? prefix : L"",
        directX, directY, rxl, rxr, ryu, ryd, lt, rt);
    joyshim::WriteLogLine(config, message);
}

void ConfigureWorkerSlot(
    joyshim::SdlBackend& backend,
    const joyshim::RuntimeConfig& config,
    WorkerSlot& slot) noexcept {
    slot.identity = {};
    slot.identity.vendor = backend.GetGamepadVendor(slot.gamepad);
    slot.identity.product = backend.GetGamepadProduct(slot.gamepad);
    slot.identity.connection = backend.GetConnectionKind(slot.gamepad);
    backend.GetGamepadGuidForId(slot.id, slot.identity.guid, sizeof(slot.identity.guid));
    CopyUtf8(slot.identity.name, backend.GetGamepadName(slot.gamepad));
    CopyUtf8(slot.identity.serial, backend.GetGamepadSerial(slot.gamepad));
    CopyUtf8(slot.identity.path, backend.GetGamepadPath(slot.gamepad));
#ifdef BSCN_SINGLE_DLL
    slot.sdlType = backend.GetGamepadType(slot.gamepad);
    // 类型字符串由输入线程生成，不让 D3D9 渲染线程进入 SDL 内部锁。
    CopyUtf8(slot.sdlTypeName,
        SDL_GetGamepadStringForType(static_cast<SDL_GamepadType>(slot.sdlType)));
    slot.stadia = bondone::IsStadia(slot.identity.vendor, slot.identity.name);
    if (!slot.stadia) {
        // 官方 SDL 映射字符串包含设备名称。只在打开设备时查询一次，
        // 有线和蓝牙即使原始 HID 名称不同，也可由 SDL 数据库识别。
        char* mapping = backend.GetGamepadMapping(slot.gamepad);
        if (mapping != nullptr) {
            slot.stadia = bondone::ContainsAscii(mapping, "stadia");
            backend.FreeMemory(mapping);
        }
    }
#endif
    slot.profile = joyshim::LoadDeviceProfile(config, slot.identity);
    slot.joystick = backend.GetGamepadJoystick(slot.gamepad);
    slot.rawAxisCount = slot.joystick != nullptr
        ? backend.GetNumJoystickAxes(slot.joystick) : 0;
    slot.rawButtonCount = slot.joystick != nullptr
        ? backend.GetNumJoystickButtons(slot.joystick) : 0;
    if (slot.rawAxisCount < 0) slot.rawAxisCount = 0;
    if (slot.rawButtonCount < 0) slot.rawButtonCount = 0;

    if (config.enableLog) {
        // 手柄子集合标识采用 UTF-8 存储，不能直接用 %hs 按系统代码页输出。
        wchar_t alias[joyshim::MaxProfileAlias]{};
        Utf8ToWide(slot.profile.controllerAlias, alias, static_cast<int>(std::size(alias)));
        wchar_t message[640]{};
        std::swprintf(
            message,
            640,
            L"手柄进入槽位：后端=%ls，GUID=%hs，VID:PID=%04X:%04X，手柄子集合=%ls，匹配依据=%ls，主项目设置=%ls，手柄覆盖=%ls，输入修正=%ls，输出修正=%ls，输出微调=%ls，原生DirectInput=%ls，重定向=%ls，无效配置=%u，原始轴=%d，原始按键=%d。",
            backend.Name(),
            slot.identity.guid[0] != '\0' ? slot.identity.guid : "(未知)",
            static_cast<unsigned int>(slot.identity.vendor),
            static_cast<unsigned int>(slot.identity.product),
            alias[0] != L'\0' ? alias : L"(无)",
            joyshim::ControllerMatchKindName(slot.profile.controllerMatch),
            slot.profile.projectPatchApplied ? L"已应用" : L"默认",
            slot.profile.controllerPatchApplied ? L"已覆盖" : L"未覆盖",
            joyshim::HasCorrection(slot.profile.correction) ? L"启用" : L"未启用",
            joyshim::HasCorrection(slot.profile.outputCorrection) ? L"启用" : L"未启用",
            joyshim::HasOutputTuning(slot.profile.outputTuning) ? L"启用" : L"未启用",
            slot.profile.nativeInput.blockDirectInput ? L"隐藏匹配设备" : L"保持可见",
            slot.profile.routing.customized ? L"启用" : L"默认",
            static_cast<unsigned int>(slot.profile.routing.invalidEntries),
            slot.rawAxisCount,
            slot.rawButtonCount);
        joyshim::WriteLogLine(config, message);
        if (slot.profile.controllerMatch == joyshim::ControllerMatchKind::GuidFallback) {
            joyshim::WriteLogLine(
                config,
                L"手柄识别提示：Serials、Paths、DeviceIds、Names 与 NameContains 均未命中，已使用 Guids 最后一层兜底。");
        }
        if (slot.profile.nativeInput.blockDirectInput) {
            const joyshim::ProxyHookSnapshot hooks = joyshim::ProxyEntryHookSnapshot();
            const bool directPath =
                joyshim::ProxyEntryCurrentKind() == joyshim::ProxyEntryKind::DInput8 ||
                hooks.directInputSlotsPatched != 0 ||
                hooks.getProcAddressSlotsPatched != 0 ||
                hooks.dynamicDirectInputRedirects != 0 ||
                hooks.directInputCreateCalls != 0;
            if (!directPath) {
                joyshim::WriteLogLine(
                    config,
                    L"警告：当前 Profile 要求隐藏 DirectInput 设备，但尚未发现 DirectInput8Create 静态、延迟或动态接管路径；将继续监视后加载游戏模块。");
            }
        }
    }

    if (config.enableLog && config.verboseDeviceInfo) {
        wchar_t name[256]{};
        wchar_t path[512]{};
        wchar_t mapping[1024]{};
        Utf8ToWide(slot.identity.name, name, 256);
        Utf8ToWide(slot.identity.path, path, 512);
        char* mappingUtf8 = backend.GetGamepadMapping(slot.gamepad);
        Utf8ToWide(mappingUtf8, mapping, 1024);
        wchar_t details[2048]{};
        std::swprintf(
            details,
            2048,
            L"设备详情：名称=%ls，GUID=%hs，序列号=%hs，路径=%ls，映射=%ls",
            name[0] != L'\0' ? name : L"(未知)",
            slot.identity.guid[0] != '\0' ? slot.identity.guid : "(未知)",
            slot.identity.serial[0] != '\0' ? slot.identity.serial : "(不可用)",
            path[0] != L'\0' ? path : L"(未知)",
            mapping[0] != L'\0' ? mapping : L"(无映射)");
        joyshim::WriteLogLine(config, details);
        if (mappingUtf8 != nullptr) backend.FreeMemory(mappingUtf8);
        LogAxisRouting(config, slot.profile.routing.axes, L"当前");
        if (joyshim::HasOutputTuning(slot.profile.outputTuning.rightStick)) {
            const auto& tuning = slot.profile.outputTuning.rightStick;
            const wchar_t* biasAxis = tuning.diagonal.axis ==
                joyshim::DiagonalBiasAxis::X ? L"X" : L"Y";
            const wchar_t* signMode = L"All";
            if (tuning.diagonal.signMode == joyshim::DiagonalSignMode::SameSigns) {
                signMode = L"SameSigns";
            } else if (tuning.diagonal.signMode ==
                joyshim::DiagonalSignMode::OppositeSigns) {
                signMode = L"OppositeSigns";
            }
            const wchar_t* separationMode = L"None";
            if (tuning.separation.mode == joyshim::AxisSeparationMode::PreferX) {
                separationMode = L"PreferX";
            } else if (tuning.separation.mode == joyshim::AxisSeparationMode::PreferY) {
                separationMode = L"PreferY";
            } else if (tuning.separation.mode == joyshim::AxisSeparationMode::DominantAxis) {
                separationMode = L"DominantAxis";
            }
            const wchar_t* separationSigns = L"All";
            if (tuning.separation.signMode == joyshim::DiagonalSignMode::SameSigns) {
                separationSigns = L"SameSigns";
            } else if (tuning.separation.signMode == joyshim::DiagonalSignMode::OppositeSigns) {
                separationSigns = L"OppositeSigns";
            }
            wchar_t tuningMessage[800]{};
            std::swprintf(
                tuningMessage,
                800,
                L"右摇杆输出微调：矩阵=[%d,%d;%d,%d]，对角线修正=%ls，容差=%d，旧偏移=%d，最小轴幅=%d，最小主轴差=%d，主轴=%ls，符号范围=%ls；轴分离模式=%ls，最小轴幅=%d，副轴比例=%d%%，符号范围=%ls。",
                tuning.matrix.xx,
                tuning.matrix.xy,
                tuning.matrix.yx,
                tuning.matrix.yy,
                tuning.diagonal.enabled ? L"启用" : L"关闭",
                tuning.diagonal.tolerance,
                tuning.diagonal.offset,
                tuning.diagonal.minimumMagnitude,
                tuning.diagonal.minimumGap,
                biasAxis,
                signMode,
                separationMode,
                tuning.separation.minimumMagnitude,
                tuning.separation.secondaryPercent,
                separationSigns);
            joyshim::WriteLogLine(config, tuningMessage);
        }
    }
}

void CloseWorkerSlot(
    joyshim::SdlBackend& backend,
    WorkerSlot& slot,
    std::size_t index) noexcept {
    if (slot.gamepad != nullptr) {
        backend.RumbleGamepad(slot.gamepad, 0, 0, 0);
        backend.CloseGamepad(slot.gamepad);
    }
    slot = {};
    ClearPublishedSlot(index);
}

bool SameDiscoveredJoysticks(
    const JoystickDiscoverySnapshot& snapshot,
    const std::array<joyshim::SdlDeviceId,
        joyshim::MaxEnumeratedGamepads>& ids,
    std::size_t count) noexcept {
    if (!snapshot.initialized || snapshot.count != count) return false;
    for (std::size_t index = 0; index < count; ++index) {
        if (snapshot.ids[index] != ids[index]) return false;
    }
    return true;
}

void LogJoystickDiscoveryIfChanged(
    joyshim::SdlBackend& backend,
    const joyshim::RuntimeConfig& config,
    JoystickDiscoverySnapshot& snapshot) noexcept {
    if (!config.enableLog) return;

    std::array<joyshim::SdlDeviceId,
        joyshim::MaxEnumeratedGamepads> ids{};
    std::size_t count = 0;
    if (!backend.EnumerateJoysticks(ids, count) ||
        SameDiscoveredJoysticks(snapshot, ids, count)) {
        return;
    }

    snapshot.ids = ids;
    snapshot.count = count;
    snapshot.initialized = true;

    wchar_t summary[160]{};
    std::swprintf(summary, 160, L"SDL3 原始 Joystick 列表变化：当前设备数=%u。",
        static_cast<unsigned int>(count));
    joyshim::WriteLogLine(config, summary);

    for (std::size_t index = 0; index < count; ++index) {
        wchar_t name[256]{};
        wchar_t path[512]{};
        Utf8ToWide(backend.GetJoystickNameForId(ids[index]), name, 256);
        Utf8ToWide(backend.GetJoystickPathForId(ids[index]), path, 512);
        char guid[33]{};
        backend.GetJoystickGuidForId(ids[index], guid, sizeof(guid));
        wchar_t message[1280]{};
        std::swprintf(
            message,
            1280,
            L"SDL3 发现 Joystick：ID=%u，Gamepad=%ls，GUID=%hs，VID:PID=%04X:%04X，名称=%ls，路径=%ls。",
            static_cast<unsigned int>(ids[index]),
            backend.IsGamepad(ids[index]) ? L"是" : L"否",
            guid[0] != '\0' ? guid : "(未知)",
            static_cast<unsigned int>(backend.GetJoystickVendorForId(ids[index])),
            static_cast<unsigned int>(backend.GetJoystickProductForId(ids[index])),
            name[0] != L'\0' ? name : L"(未知)",
            path[0] != L'\0' ? path : L"(未知)");
        joyshim::WriteLogLine(config, message);
    }
}

void ReconcileWorkerSlots(
    joyshim::SdlBackend& backend,
    const joyshim::RuntimeConfig& config,
    std::array<WorkerSlot, joyshim::MaxXInputUsers>& slots,
    JoystickDiscoverySnapshot& discovery,
    ULONGLONG& nextScanTime,
    bool forceScan) noexcept {
    bool disconnected = false;
    for (std::size_t index = 0; index < slots.size(); ++index) {
        WorkerSlot& slot = slots[index];
        if (slot.gamepad != nullptr && !backend.GamepadConnected(slot.gamepad)) {
            CloseWorkerSlot(backend, slot, index);
            disconnected = true;
        }
    }

    const ULONGLONG now = GetTickCount64();
    if (!forceScan && !disconnected && now < nextScanTime) return;
    nextScanTime = now + RescanIntervalMs;
    LogJoystickDiscoveryIfChanged(backend, config, discovery);

    std::array<joyshim::SdlDeviceId, joyshim::MaxEnumeratedGamepads> ids{};
    std::size_t count = 0;
    if (!backend.Enumerate(ids, count)) return;

    joyshim::SlotIds current{};
    for (std::size_t index = 0; index < slots.size(); ++index) current[index] = slots[index].id;
    const auto desired = joyshim::ReconcileSlotIds(current, ids.data(), count);

    for (std::size_t index = 0; index < slots.size(); ++index) {
        WorkerSlot& slot = slots[index];
        if (slot.id == desired[index]) continue;
        CloseWorkerSlot(backend, slot, index);
        if (desired[index] == 0) continue;
        const joyshim::SdlGamepadHandle opened = backend.OpenGamepad(desired[index]);
        if (opened == nullptr) continue;
        slot.id = desired[index];
        slot.gamepad = opened;
        ConfigureWorkerSlot(backend, config, slot);
    }
}

class WorkerInputReader {
public:
    WorkerInputReader(joyshim::SdlBackend& backend, const WorkerSlot& slot) noexcept
        : backend_(backend), slot_(slot) {
        Capture();
    }

    bool ReadButton(const joyshim::ButtonSource& source) const noexcept {
        if (source.kind == joyshim::ButtonSourceKind::Standard) {
            return source.index < standardButtons_.size() && standardButtons_[source.index];
        }
        if (source.kind == joyshim::ButtonSourceKind::Raw) {
            return source.index < rawButtonCount_ && rawButtons_[source.index];
        }
        return false;
    }

    std::int16_t ReadAxis(const joyshim::HalfAxisSource& source) const noexcept {
        if (source.kind == joyshim::AxisSourceKind::Standard) {
            if (source.index >= static_cast<std::uint16_t>(joyshim::StandardAxis::Count)) {
                return 0;
            }
            return standardAxes_.Get(static_cast<joyshim::StandardAxis>(source.index));
        }
        if (source.kind == joyshim::AxisSourceKind::Raw) {
            return source.index < rawAxisCount_ ? rawAxes_[source.index] : 0;
        }
        return 0;
    }

private:
    void Capture() noexcept {
        // 所有 SDL 查询都发生在唯一输入线程中；一次循环先手动更新控制器，再把
        // 标准轴、Raw.Axis 和按键完整复制到本地不可变快照。XInput 调用线程
        // 永远不会接触 SDL，也不会让 X/Y 落入两个不同的 SDL 更新时间点。
        for (std::size_t index = 0; index < standardButtons_.size(); ++index) {
            standardButtons_[index] = backend_.GetGamepadButton(
                slot_.gamepad, static_cast<int>(index));
        }

        const joyshim::AxisPair left = joyshim::ApplyAxisCorrection(
            backend_.GetGamepadAxis(slot_.gamepad, 0),
            backend_.GetGamepadAxis(slot_.gamepad, 1),
            slot_.profile.correction.leftStick);
        const joyshim::AxisPair right = joyshim::ApplyAxisCorrection(
            backend_.GetGamepadAxis(slot_.gamepad, 2),
            backend_.GetGamepadAxis(slot_.gamepad, 3),
            slot_.profile.correction.rightStick);
        standardAxes_.leftX = left.x;
        standardAxes_.leftY = left.y;
        standardAxes_.rightX = right.x;
        standardAxes_.rightY = right.y;
        standardAxes_.leftTrigger = backend_.GetGamepadAxis(slot_.gamepad, 4);
        standardAxes_.rightTrigger = backend_.GetGamepadAxis(slot_.gamepad, 5);

        const int boundedAxes = slot_.rawAxisCount > static_cast<int>(rawAxes_.size())
            ? static_cast<int>(rawAxes_.size()) : slot_.rawAxisCount;
        rawAxisCount_ = boundedAxes > 0 ? static_cast<std::size_t>(boundedAxes) : 0;
        for (std::size_t index = 0; index < rawAxisCount_; ++index) {
            rawAxes_[index] = backend_.GetJoystickAxis(slot_.joystick, static_cast<int>(index));
        }

        const int boundedButtons = slot_.rawButtonCount > static_cast<int>(rawButtons_.size())
            ? static_cast<int>(rawButtons_.size()) : slot_.rawButtonCount;
        rawButtonCount_ = boundedButtons > 0 ? static_cast<std::size_t>(boundedButtons) : 0;
        for (std::size_t index = 0; index < rawButtonCount_; ++index) {
            rawButtons_[index] = backend_.GetJoystickButton(
                slot_.joystick, static_cast<int>(index));
        }
    }

    joyshim::SdlBackend& backend_;
    const WorkerSlot& slot_;
    std::array<bool, static_cast<std::size_t>(
        joyshim::StandardButton::Count)> standardButtons_{};
    joyshim::StandardAxisValues standardAxes_{};
    std::array<std::int16_t, 256> rawAxes_{};
    std::array<bool, 256> rawButtons_{};
    std::size_t rawAxisCount_{};
    std::size_t rawButtonCount_{};
};

XINPUT_GAMEPAD CaptureWorkerState(
    joyshim::SdlBackend& backend,
    WorkerSlot& slot,
    const joyshim::RuntimeConfig& config,
    bool enabled) noexcept {
    XINPUT_GAMEPAD output{};
    if (!enabled) return output;

    WorkerInputReader reader(backend, slot);
    const joyshim::RoutedState routed = joyshim::EvaluateInputRouting(
        slot.profile.routing,
        [&](const joyshim::ButtonSource& source) noexcept {
            return reader.ReadButton(source);
        },
        [&](const joyshim::HalfAxisSource& source) noexcept {
            return reader.ReadAxis(source);
        });
    const joyshim::AxisPair correctedLeft = joyshim::ApplyAxisCorrection(
        routed.leftX, routed.leftY, slot.profile.outputCorrection.leftStick);
    const joyshim::AxisPair correctedRight = joyshim::ApplyAxisCorrection(
        routed.rightX, routed.rightY, slot.profile.outputCorrection.rightStick);

    output.wButtons = static_cast<WORD>(routed.buttons);
    output.bLeftTrigger = routed.leftTrigger;
    output.bRightTrigger = routed.rightTrigger;
    const joyshim::AxisPair tunedLeft = joyshim::ApplyAxisOutputTuning(
        correctedLeft.x, correctedLeft.y, slot.profile.outputTuning.leftStick);
    const joyshim::AxisPair tunedRight = joyshim::ApplyAxisOutputTuning(
        correctedRight.x, correctedRight.y, slot.profile.outputTuning.rightStick);

    output.sThumbLX = tunedLeft.x;
    output.sThumbLY = tunedLeft.y;
    output.sThumbRX = tunedRight.x;
    output.sThumbRY = tunedRight.y;

    // 仅在显式启用输出微调并打开详细日志时记录样本。该诊断不会改变
    // 输入值，也不会进入默认透明路径。250 ms 节流用于避免高频日志影响游戏。
    if (config.enableLog && config.verboseDeviceInfo &&
        joyshim::HasOutputTuning(slot.profile.outputTuning.rightStick)) {
        const ULONGLONG now = GetTickCount64();
        if (now >= slot.nextRightStickTraceTime) {
            const auto& diagonal = slot.profile.outputTuning.rightStick.diagonal;
            const std::int64_t magnitudeX = joyshim::AxisMagnitude(correctedRight.x);
            const std::int64_t magnitudeY = joyshim::AxisMagnitude(correctedRight.y);
            const std::int64_t minimumToLog = diagonal.minimumMagnitude > 0
                ? diagonal.minimumMagnitude : 4096;
            if (magnitudeX >= minimumToLog && magnitudeY >= minimumToLog) {
                const std::int64_t difference = magnitudeX >= magnitudeY
                    ? magnitudeX - magnitudeY : magnitudeY - magnitudeX;
                const bool sameSigns = (correctedRight.x < 0) == (correctedRight.y < 0);
                const bool changed = correctedRight.x != tunedRight.x ||
                    correctedRight.y != tunedRight.y;
                wchar_t trace[640]{};
                std::swprintf(
                    trace,
                    640,
                    L"右摇杆样本：修正前=(%d,%d)，最终=(%d,%d)，幅值差=%lld，符号=%ls，已改变=%ls。",
                    static_cast<int>(correctedRight.x),
                    static_cast<int>(correctedRight.y),
                    static_cast<int>(tunedRight.x),
                    static_cast<int>(tunedRight.y),
                    static_cast<long long>(difference),
                    sameSigns ? L"同号" : L"异号",
                    changed ? L"是" : L"否");
                joyshim::WriteLogLine(config, trace);
                slot.nextRightStickTraceTime = now + 250;
            }
        }
    }
    return output;
}

void ApplyWorkerRumble(
    joyshim::SdlBackend& backend,
    WorkerSlot& slot,
    const RumbleCommand& command,
    bool enabled) noexcept {
    if (slot.gamepad == nullptr || slot.appliedRumbleGeneration == command.generation) return;
    const std::uint16_t left = enabled ? command.left : 0;
    const std::uint16_t right = enabled ? command.right : 0;
    const std::uint32_t duration = (left == 0 && right == 0) ? 0u : ContinuousRumbleMs;
    backend.RumbleGamepad(slot.gamepad, left, right, duration);
    slot.appliedRumbleGeneration = command.generation;
}

DWORD WINAPI InputWorker(void*) noexcept {
    joyshim::RuntimeConfig config{};
    std::uint32_t configGeneration = 0;
    std::uint32_t mappingGeneration = 0;
    std::array<RumbleCommand, joyshim::MaxXInputUsers> rumble{};
    AcquireSRWLockShared(&g_controlLock);
    config = g_config;
    configGeneration = g_configGeneration;
    mappingGeneration = g_mappingReloadGeneration;
    rumble = g_rumbleCommands;
    ReleaseSRWLockShared(&g_controlLock);

    joyshim::SdlBackend backend;
    wchar_t detail[512]{};
    joyshim::WriteLogLine(config, L"开始初始化独立 SDL3 输入线程。默认路径只执行 SDL3 标准 Gamepad 到 XInput 的透明转换。");
    if (!backend.Initialize(config, detail, 512)) {
        wchar_t message[640]{};
        std::swprintf(message, 640, L"错误：%ls", detail[0] != L'\0' ? detail : L"SDL 后端初始化失败。");
        joyshim::WriteLogLine(config, message);
        SetEvent(g_inputReadyEvent);
        return 0;
    }
    joyshim::WriteLogLine(config, detail);
    const joyshim::MappingLoadResult initialMapping =
        joyshim::LoadConfiguredMappingDatabase(config, backend);
    if (initialMapping.requested) joyshim::WriteLogLine(config, initialMapping.detail);

    std::array<WorkerSlot, joyshim::MaxXInputUsers> slots{};
    JoystickDiscoverySnapshot discovery{};
    ULONGLONG nextScanTime = 0;

    // 首次 XInput 查询前必须完成一次手动控制器更新、设备枚举和状态发布。
    // 否则游戏可能在输入线程刚启动但尚未扫描到手柄的极短窗口内收到
    // ERROR_DEVICE_NOT_CONNECTED，并在整个会话中不再重新探测控制器。
    backend.UpdateGamepads();
    ReconcileWorkerSlots(backend, config, slots, discovery, nextScanTime, true);
    const bool initiallyEnabled =
        InterlockedCompareExchange(&g_enabled, FALSE, FALSE) == TRUE;
    std::size_t initialConnectedCount = 0;
    for (std::size_t index = 0; index < slots.size(); ++index) {
        if (slots[index].gamepad == nullptr) continue;
        ++initialConnectedCount;
        ApplyWorkerRumble(backend, slots[index], rumble[index], initiallyEnabled && config.enableRumble);
        PublishWorkerSlot(
            index,
            slots[index],
            CaptureWorkerState(backend, slots[index], config, initiallyEnabled),
            backend.CanRumble() && config.enableRumble);
    }
    UpdateManagedInputSnapshot(slots);
    if (initialConnectedCount == 0) {
        joyshim::WriteLogLine(
            config,
            L"SDL3 初始化完成：启动时未连接手柄；输入线程保持运行并等待热插拔。");
    }

    InterlockedExchange(&g_initialized, TRUE);
    SetEvent(g_inputReadyEvent);

    bool forceScan = false;
    for (;;) {
        if (WaitForSingleObject(g_inputStopEvent, 0) == WAIT_OBJECT_0) break;

        std::uint32_t nextConfigGeneration = 0;
        std::uint32_t nextMappingGeneration = 0;
        std::array<RumbleCommand, joyshim::MaxXInputUsers> nextRumble{};
        bool replaceConfig = false;
        AcquireSRWLockShared(&g_controlLock);
        nextConfigGeneration = g_configGeneration;
        nextMappingGeneration = g_mappingReloadGeneration;
        nextRumble = g_rumbleCommands;
        if (nextConfigGeneration != configGeneration) {
            // RuntimeConfig 内含动态 Profile 容器。只有配置代次改变时才复制，
            // 禁止在通常为 1 ms 的输入轮询中持续深拷贝整棵 TOML 配置树。
            config = g_config;
            replaceConfig = true;
        }
        ReleaseSRWLockShared(&g_controlLock);

        if (replaceConfig) {
            configGeneration = nextConfigGeneration;
            for (WorkerSlot& slot : slots) {
                if (slot.gamepad == nullptr) continue;
                // 专版不支持配置热重载；设备类型与 Stadia 判定在打开时已固定。
                slot.profile = joyshim::LoadDeviceProfile(config, slot.identity);
                // 热重载可能刚刚关闭振动。强制向物理手柄发送一次更新，
                // 不能因游戏没有再次调用 XInputSetState 而持续震动。
                slot.appliedRumbleGeneration = ~std::uint32_t{0};
            }
            forceScan = true;
        }
        rumble = nextRumble;

        if (nextMappingGeneration != mappingGeneration) {
            mappingGeneration = nextMappingGeneration;
            const joyshim::MappingLoadResult result =
                joyshim::LoadConfiguredMappingDatabase(config, backend);
            if (result.requested) joyshim::WriteLogLine(config, result.detail);
            forceScan = true;
        }

        // SDL 的手动设备更新、枚举、状态读取、震动和关闭全部固定在此线程。
        // 禁止在工作线程调用 SDL_PumpEvents；它属于宿主主线程的事件循环。
        backend.UpdateGamepads();
        // 只在已离开 loader lock 的独立 SDL 输入线程中监视后加载的游戏
        // 模块。该操作内部按 1 秒节流，且只改写游戏目录模块。
        joyshim::ProxyEntryRefreshHooks(config);
        ReconcileWorkerSlots(backend, config, slots, discovery, nextScanTime, forceScan);
        forceScan = false;

        const bool enabled = InterlockedCompareExchange(&g_enabled, FALSE, FALSE) == TRUE;
        for (std::size_t index = 0; index < slots.size(); ++index) {
            WorkerSlot& slot = slots[index];
            if (slot.gamepad == nullptr) continue;
            ApplyWorkerRumble(backend, slot, rumble[index], enabled && config.enableRumble);
            const XINPUT_GAMEPAD state = CaptureWorkerState(backend, slot, config, enabled);
            PublishWorkerSlot(index, slot, state, backend.CanRumble() && config.enableRumble);
        }

        UpdateManagedInputSnapshot(slots);
        HANDLE waits[2] = {g_inputStopEvent, g_inputWakeEvent};
        const DWORD waitResult = WaitForMultipleObjects(
            2, waits, FALSE, config.inputPollIntervalMs);
        if (waitResult == WAIT_OBJECT_0) break;
    }

    for (std::size_t index = 0; index < slots.size(); ++index) {
        CloseWorkerSlot(backend, slots[index], index);
    }
    UpdateManagedInputSnapshot(slots);
    backend.Shutdown();
    InterlockedExchange(&g_initialized, FALSE);
    return 0;
}

void UpdateManagedInputSnapshot(
    const std::array<WorkerSlot, joyshim::MaxXInputUsers>& slots) noexcept {
    // 只有已经打开且获得非零 SDL 实例 ID 的设备才进入快照；
    // DirectInput 过滤据此判断当前游戏是否能通过 JoyShim 获取对应 XInput。
    std::array<joyshim::ManagedDeviceIdentity, joyshim::MaxXInputUsers> managed{};
    std::size_t count = 0;
    for (const WorkerSlot& slot : slots) {
        if (slot.gamepad == nullptr || slot.id == 0 ||
            !slot.identity.path[0] || !slot.identity.vendor || !slot.identity.product) continue;
        auto& item = managed[count++];
        item.vendor = slot.identity.vendor;
        item.product = slot.identity.product;
#ifdef BSCN_SINGLE_DLL
        // 由 SDL 打开设备时保留的真实名称/厂商确认 Stadia，不依赖 DI 别名。
        item.stadia = slot.stadia;
#endif
        CopyUtf8(item.path, slot.identity.path);
    }
    joyshim::PublishManagedDevices(managed, count);
}

BOOL CALLBACK InitializeOnce(PINIT_ONCE, PVOID, PVOID*) noexcept {
    g_config = joyshim::LoadRuntimeConfig(g_module);
    if (g_config.enableLog) {
        wchar_t configMessage[640]{};
        std::swprintf(
            configMessage,
            640,
            L"配置来源：模式=%ls，当前进程=%ls，路径=%ls，专版主 EXE=%ls，目录默认配置=%ls，解析有效=%ls。",
            ConfigSourceName(g_config.configSource),
            g_config.processName[0] != L'\0' ? g_config.processName : L"(未知)",
            g_config.tomlPath[0] != L'\0' ? g_config.tomlPath : L"(未知)",
            g_config.selectedProfileOwner[0] != L'\0' ? g_config.selectedProfileOwner : L"(无)",
            g_config.genericConfigIgnored ? L"因专版生效而跳过" : L"未被专版跳过",
            g_config.configValid ? L"是" : L"否");
        joyshim::WriteLogLine(g_config, configMessage);
    }
    if (!g_config.configValid || g_config.configReadFailed) {
        wchar_t message[768]{};
        std::swprintf(
            message,
            768,
            L"JoyShim TOML 配置未生效：第 %u 行第 %u 列，字段路径=%ls，错误=%ls，修复建议=%ls。错误报告=%ls\n",
            static_cast<unsigned int>(g_config.parseErrorLine),
            static_cast<unsigned int>(g_config.parseErrorColumn),
            g_config.parseErrorPath[0] != L'\0' ? g_config.parseErrorPath : L"(未知)",
            g_config.parseError[0] != L'\0' ? g_config.parseError : L"读取或解析失败",
            g_config.parseErrorSuggestion[0] != L'\0' ? g_config.parseErrorSuggestion : L"参考默认配置",
            g_config.errorReportPath[0] != L'\0' ? g_config.errorReportPath : L"(未生成)");
        OutputDebugStringW(message);
        joyshim::WriteLogLine(g_config, message);
    }

    // 八种代理名称进入同一条接管链。无论当前入口是 XInput、dinput8、
    // winmm 等入口统一处理普通导入、延迟导入和受控的
    // GetProcAddress 动态解析；配置只决定是否启用 DirectInput 去重。
    joyshim::ProxyEntryConfigureHooks(
        g_config,
        g_config.configValid && joyshim::ConfigRequestsDirectInputInterception(g_config));

    g_inputStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_inputWakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_inputReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_inputStopEvent == nullptr || g_inputWakeEvent == nullptr ||
        g_inputReadyEvent == nullptr) {
        joyshim::WriteLogLine(g_config, L"错误：无法创建独立 SDL 输入线程事件对象。");
        return TRUE;
    }

    HANDLE thread = CreateThread(nullptr, 0, InputWorker, nullptr, 0, nullptr);
    if (thread == nullptr) {
        joyshim::WriteLogLine(g_config, L"错误：无法创建独立 SDL 输入线程。");
        return TRUE;
    }
    CloseHandle(thread);

    if (WaitForSingleObject(g_inputReadyEvent, InputStartupTimeoutMs) != WAIT_OBJECT_0) {
        joyshim::WriteLogLine(g_config, L"错误：独立 SDL 输入线程初始化超时。");
        SetEvent(g_inputStopEvent);
        return TRUE;
    }
    if (InterlockedCompareExchange(&g_initialized, FALSE, FALSE) == TRUE &&
        g_config.enableHotReload) {
        StartHotReloadWatcher();
    }
    return TRUE;
}

bool EnsureInitialized() noexcept {
    const LONG current = InterlockedCompareExchange(
        &g_runtimeState, RuntimeNotStarted, RuntimeNotStarted);
    if (current == RuntimeReady) return true;
    if (current == RuntimeInitializing || current == RuntimeFailed) return false;

    // 只有一个调用者可以负责启动。SDL3 加载期间可能重入本地
    // winmm 代理；重入调用看到 RuntimeInitializing 后必须立即
    // 透明转发系统 API，不能等待同一轮初始化，否则会形成线程死锁。
    const LONG previous = InterlockedCompareExchange(
        &g_runtimeState, RuntimeInitializing, RuntimeNotStarted);
    if (previous != RuntimeNotStarted) return previous == RuntimeReady;

    InitOnceExecuteOnce(&g_initOnce, InitializeOnce, nullptr, nullptr);
    const bool initialized =
        InterlockedCompareExchange(&g_initialized, FALSE, FALSE) == TRUE;
    InterlockedExchange(
        &g_runtimeState,
        initialized ? RuntimeReady : RuntimeFailed);
    return initialized;
}


bool ApplyReloadedConfiguration(
    const joyshim::RuntimeConfig& newConfig,
    bool reloadMappingDatabase) noexcept {
    const joyshim::RuntimeConfig oldConfig = CopyRuntimeConfig();
    if (!newConfig.configValid) {
        wchar_t message[768]{};
        std::swprintf(
            message,
            768,
            L"TOML 热重载失败：第 %u 行第 %u 列，字段路径=%ls，错误=%ls，修复建议=%ls；继续使用上一份有效配置。错误报告=%ls。",
            static_cast<unsigned int>(newConfig.parseErrorLine),
            static_cast<unsigned int>(newConfig.parseErrorColumn),
            newConfig.parseErrorPath[0] != L'\0' ? newConfig.parseErrorPath : L"(未知)",
            newConfig.parseError[0] != L'\0' ? newConfig.parseError : L"配置解析失败",
            newConfig.parseErrorSuggestion[0] != L'\0' ? newConfig.parseErrorSuggestion : L"参考默认配置",
            newConfig.errorReportPath[0] != L'\0' ? newConfig.errorReportPath : L"(未生成)");
        joyshim::WriteLogLine(oldConfig.enableLog ? oldConfig : newConfig, message);
        return false;
    }

    joyshim::ProxyEntryConfigureHooks(
        newConfig,
        joyshim::ConfigRequestsDirectInputInterception(newConfig));

    std::array<PublishedSlot, joyshim::MaxXInputUsers> published{};
    AcquireSRWLockShared(&g_stateLock);
    published = g_publishedSlots;
    ReleaseSRWLockShared(&g_stateLock);

    std::array<joyshim::DeviceProfile, joyshim::MaxXInputUsers> profiles{};
    std::uint32_t invalidEntries = 0;
    unsigned int activeCount = 0;
    for (std::size_t index = 0; index < published.size(); ++index) {
        if (!published[index].connected) continue;
        ++activeCount;
        profiles[index] = joyshim::LoadDeviceProfile(
            newConfig, published[index].identity);
        invalidEntries += profiles[index].routing.invalidEntries;
    }

    if (invalidEntries != 0) {
        wchar_t message[256]{};
        std::swprintf(
            message,
            256,
            L"配置热重载失败：活动设备配置包含 %u 个无效按键/轴条目，继续使用旧配置。",
            static_cast<unsigned int>(invalidEntries));
        joyshim::WriteLogLine(oldConfig.enableLog ? oldConfig : newConfig, message);
        return false;
    }

    AcquireSRWLockExclusive(&g_controlLock);
    g_config = newConfig;
    ++g_configGeneration;
    if (reloadMappingDatabase && newConfig.enableExternalDatabase &&
        newConfig.reloadMappingDatabase) {
        ++g_mappingReloadGeneration;
    }
    ReleaseSRWLockExclusive(&g_controlLock);
    if (g_inputWakeEvent != nullptr) SetEvent(g_inputWakeEvent);

    wchar_t message[384]{};
    std::swprintf(
        message,
        384,
        L"配置热重载成功：活动槽位=%u，日志=%ls，GuideGetState=%ls；配置由独立输入线程应用。",
        activeCount,
        newConfig.enableLog ? L"启用" : L"关闭",
        newConfig.exposeGuideInGetState ? L"启用" : L"关闭");
    joyshim::WriteLogLine(newConfig.enableLog ? newConfig : oldConfig, message);

    for (std::size_t index = 0; index < profiles.size(); ++index) {
        if (published[index].connected) {
            LogAxisRouting(newConfig, profiles[index].routing.axes, L"热重载后");
        }
    }
    return true;
}

DWORD WINAPI HotReloadWorker(void*) noexcept {
    joyshim::RuntimeConfig current = CopyRuntimeConfig();
    if (current.hotReloadStartupDelayMs != 0 &&
        WaitForSingleObject(g_hotReloadStopEvent, current.hotReloadStartupDelayMs) ==
            WAIT_OBJECT_0) return 0;

    FileStamp tomlStamp = QueryFileStamp(current.tomlPath);
    FileStamp databaseStamp = QueryFileStamp(current.mappingDatabasePath);
    for (;;) {
        current = CopyRuntimeConfig();
        if (!current.enableHotReload) return 0;
        if (WaitForSingleObject(g_hotReloadStopEvent, current.hotReloadPollMs) ==
            WAIT_OBJECT_0) return 0;

        const FileStamp nextTomlStamp = QueryFileStamp(current.tomlPath);
        if (!SameFileStamp(nextTomlStamp, tomlStamp)) {
            if (current.hotReloadDebounceMs != 0 &&
                WaitForSingleObject(g_hotReloadStopEvent, current.hotReloadDebounceMs) ==
                    WAIT_OBJECT_0) return 0;
            const joyshim::RuntimeConfig next =
                joyshim::ReloadRuntimeConfig(g_module, current);
            const bool applied = ApplyReloadedConfiguration(next, true);

            // 解析或校验失败时，ApplyReloadedConfiguration 会保留上一份有效配置。
            // 此时不能读取失败对象里的默认 enableHotReload=false 来结束监视线程，
            // 否则用户修正 TOML 后将无法自动恢复。无论成功与否，都先记住当前
            // 文件时间戳；失败后继续等待用户下一次保存。
            tomlStamp = QueryFileStamp(next.tomlPath);
            if (!applied) continue;

            current = CopyRuntimeConfig();
            if (!current.enableHotReload) return 0;
            databaseStamp = QueryFileStamp(current.mappingDatabasePath);
            continue;
        }

        if (current.enableHotReload && current.reloadMappingDatabase &&
            current.enableExternalDatabase) {
            const FileStamp nextDatabaseStamp = QueryFileStamp(current.mappingDatabasePath);
            if (!SameFileStamp(nextDatabaseStamp, databaseStamp)) {
                if (current.hotReloadDebounceMs != 0 &&
                    WaitForSingleObject(g_hotReloadStopEvent, current.hotReloadDebounceMs) ==
                        WAIT_OBJECT_0) return 0;
                ApplyReloadedConfiguration(current, true);
                databaseStamp = QueryFileStamp(current.mappingDatabasePath);
            }
        } else {
            databaseStamp = QueryFileStamp(current.mappingDatabasePath);
        }
    }
}

void StartHotReloadWatcher() noexcept {
    if (g_hotReloadStopEvent != nullptr) return;
    g_hotReloadStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_hotReloadStopEvent == nullptr) {
        joyshim::WriteLogLine(g_config, L"警告：无法创建配置热重载停止事件，热重载不可用。");
        return;
    }
    HANDLE thread = CreateThread(nullptr, 0, HotReloadWorker, nullptr, 0, nullptr);
    if (thread == nullptr) {
        joyshim::WriteLogLine(g_config, L"警告：无法创建配置热重载线程，热重载不可用。");
        CloseHandle(g_hotReloadStopEvent);
        g_hotReloadStopEvent = nullptr;
        return;
    }
    CloseHandle(thread);
    joyshim::WriteLogLine(
        g_config,
        L"配置热重载监视已启动；重载线程只解析文件，SDL 配置由独立输入线程接管。");
}

// XInput 函数可能被其他模块的 DllMain 调用。禁止从该调用栈同步
// 解析 TOML、加载 SDL3 或 WaitForSingleObject(输入线程)。在运行时尚未
// 就绪时返回 DEVICE_NOT_CONNECTED，后台线程会在加载器锁外启动。
bool RuntimeReadyOrQueue() noexcept {
    if (InterlockedCompareExchange(&g_runtimeState, RuntimeReady, RuntimeReady) ==
        RuntimeReady) return true;
    joyshim::ProxyEntryQueueRuntimeBootstrap();
    return false;
}

bool CopyPublishedSlot(DWORD userIndex, PublishedSlot& output) noexcept {
    if (userIndex >= g_publishedSlots.size()) return false;
    AcquireSRWLockShared(&g_stateLock);
    output = g_publishedSlots[userIndex];
    ReleaseSRWLockShared(&g_stateLock);
    return output.connected;
}

void TraceReturnedState(
    DWORD userIndex,
    const wchar_t* entryName,
    const PublishedSlot& published,
    const XINPUT_STATE& state) noexcept {
    if (!published.outputTuningActive || userIndex >= XUSER_MAX_COUNT) return;

    bool loggingEnabled = false;
    bool verboseEnabled = false;
    AcquireSRWLockShared(&g_controlLock);
    loggingEnabled = g_config.enableLog;
    verboseEnabled = g_config.verboseDeviceInfo;
    ReleaseSRWLockShared(&g_controlLock);
    if (!loggingEnabled || !verboseEnabled) return;

    const bool extended = entryName != nullptr &&
        std::wcscmp(entryName, L"XInputGetStateEx") == 0;
    const ULONGLONG now = GetTickCount64();
    bool shouldLog = false;
    std::uint64_t calls = 0;
    AcquireSRWLockExclusive(&g_readTraceLock);
    if (extended) {
        calls = ++g_getStateExCallCount[userIndex];
    } else {
        calls = ++g_getStateCallCount[userIndex];
    }
    if (now >= g_nextReadTraceTime[userIndex]) {
        g_nextReadTraceTime[userIndex] = now + 2000;
        shouldLog = true;
    }
    ReleaseSRWLockExclusive(&g_readTraceLock);
    if (!shouldLog) return;

    const joyshim::RuntimeConfig config = CopyRuntimeConfig();
    wchar_t controller[128]{};
    Utf8ToWide(published.controllerAlias, controller, 128);
    wchar_t message[768]{};
    std::swprintf(
        message,
        768,
        L"XInput 实际返回：入口=%ls，用户=%u，累计调用=%llu，Packet=%u，RX=%d，RY=%d，LX=%d，LY=%d，按钮=0x%04X，手柄子集合=%ls，主项目设置=%ls，手柄覆盖=%ls。",
        entryName != nullptr ? entryName : L"(未知)",
        static_cast<unsigned int>(userIndex),
        static_cast<unsigned long long>(calls),
        static_cast<unsigned int>(state.dwPacketNumber),
        static_cast<int>(state.Gamepad.sThumbRX),
        static_cast<int>(state.Gamepad.sThumbRY),
        static_cast<int>(state.Gamepad.sThumbLX),
        static_cast<int>(state.Gamepad.sThumbLY),
        static_cast<unsigned int>(state.Gamepad.wButtons),
        controller[0] != L'\0' ? controller : L"(无)",
        published.projectPatchApplied ? L"已应用" : L"默认",
        published.controllerPatchApplied ? L"已覆盖" : L"未覆盖");
    joyshim::WriteLogLine(config, message);
}

void LogXInputEntryOnce(const wchar_t* entryName) noexcept {
    if (InterlockedCompareExchange(&g_xinputEntryReported, TRUE, FALSE) != FALSE) return;
    const joyshim::RuntimeConfig config = CopyRuntimeConfig();
    wchar_t message[512]{};
    std::swprintf(
        message,
        std::size(message),
        L"XInput 调用已进入 JoyShim：代理入口=%ls，函数=%ls。",
        joyshim::ProxyEntryCurrentName(),
        entryName != nullptr ? entryName : L"(未知)");
    joyshim::WriteLogLine(config, message);
}

DWORD ReadState(
    DWORD userIndex,
    XINPUT_STATE* state,
    bool includeGuide,
    const wchar_t* entryName) noexcept {
    if (state == nullptr || userIndex >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    std::memset(state, 0, sizeof(*state));
    if (!RuntimeReadyOrQueue()) return ERROR_DEVICE_NOT_CONNECTED;
    LogXInputEntryOnce(entryName);

    PublishedSlot published{};
    if (!CopyPublishedSlot(userIndex, published)) return ERROR_DEVICE_NOT_CONNECTED;
    state->dwPacketNumber = published.packetNumber;
    state->Gamepad = published.gamepad;
    if (!includeGuide && !ExposeGuideInGetState()) {
        state->Gamepad.wButtons &= static_cast<WORD>(~joyshim::Guide);
    }
    TraceReturnedState(userIndex, entryName, published, *state);
    return ERROR_SUCCESS;
}

DWORD CheckConnected(DWORD userIndex) noexcept {
    if (userIndex >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (!RuntimeReadyOrQueue()) return ERROR_DEVICE_NOT_CONNECTED;
    PublishedSlot published{};
    return CopyPublishedSlot(userIndex, published)
        ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

}  // namespace

namespace joyshim {

bool EnsureRuntimeInitialized() noexcept {
    return ::EnsureInitialized();
}

FARPROC ResolveLocalXInputExport(LPCSTR nameOrOrdinal) noexcept {
    if (nameOrOrdinal == nullptr) return nullptr;
    if (IS_INTRESOURCE(nameOrOrdinal)) {
        switch (reinterpret_cast<ULONG_PTR>(nameOrOrdinal)) {
            case 2: return joyshim::FunctionToFarProc(&XInputGetState);
            case 3: return joyshim::FunctionToFarProc(&XInputSetState);
            case 4: return joyshim::FunctionToFarProc(&XInputGetCapabilities);
            case 5: return joyshim::FunctionToFarProc(&XInputEnable);
            case 6: return joyshim::FunctionToFarProc(&XInputGetDSoundAudioDeviceGuids);
            case 7: return joyshim::FunctionToFarProc(&XInputGetBatteryInformation);
            case 8: return joyshim::FunctionToFarProc(&XInputGetKeystroke);
            case 100: return joyshim::FunctionToFarProc(&XInputGetStateEx);
            case 101: return joyshim::FunctionToFarProc(&XInputWaitForGuideButton);
            case 102: return joyshim::FunctionToFarProc(&XInputCancelGuideButtonWait);
            case 103: return joyshim::FunctionToFarProc(&XInputPowerOffController);
            case 108: return joyshim::FunctionToFarProc(&JoyShimGetCapabilitiesEx);
            default: return nullptr;
        }
    }
    if (std::strcmp(nameOrOrdinal, "XInputGetState") == 0) return joyshim::FunctionToFarProc(&XInputGetState);
    if (std::strcmp(nameOrOrdinal, "XInputGetStateEx") == 0) return joyshim::FunctionToFarProc(&XInputGetStateEx);
    if (std::strcmp(nameOrOrdinal, "XInputSetState") == 0) return joyshim::FunctionToFarProc(&XInputSetState);
    if (std::strcmp(nameOrOrdinal, "XInputGetCapabilities") == 0) return joyshim::FunctionToFarProc(&XInputGetCapabilities);
    if (std::strcmp(nameOrOrdinal, "XInputGetCapabilitiesEx") == 0) return joyshim::FunctionToFarProc(&JoyShimGetCapabilitiesEx);
    if (std::strcmp(nameOrOrdinal, "XInputEnable") == 0) return joyshim::FunctionToFarProc(&XInputEnable);
    if (std::strcmp(nameOrOrdinal, "XInputGetDSoundAudioDeviceGuids") == 0) return joyshim::FunctionToFarProc(&XInputGetDSoundAudioDeviceGuids);
    if (std::strcmp(nameOrOrdinal, "XInputGetAudioDeviceIds") == 0) return joyshim::FunctionToFarProc(&XInputGetAudioDeviceIds);
    if (std::strcmp(nameOrOrdinal, "XInputGetBatteryInformation") == 0) return joyshim::FunctionToFarProc(&XInputGetBatteryInformation);
    if (std::strcmp(nameOrOrdinal, "XInputGetKeystroke") == 0) return joyshim::FunctionToFarProc(&XInputGetKeystroke);
    if (std::strcmp(nameOrOrdinal, "XInputWaitForGuideButton") == 0) return joyshim::FunctionToFarProc(&XInputWaitForGuideButton);
    if (std::strcmp(nameOrOrdinal, "XInputCancelGuideButtonWait") == 0) return joyshim::FunctionToFarProc(&XInputCancelGuideButtonWait);
    if (std::strcmp(nameOrOrdinal, "XInputPowerOffController") == 0) return joyshim::FunctionToFarProc(&XInputPowerOffController);
    return nullptr;
}

}  // namespace joyshim

extern "C" DWORD WINAPI XInputGetState(DWORD userIndex, XINPUT_STATE* state) JOYSHIM_XINPUT_SDK_NOEXCEPT {
    return ReadState(userIndex, state, false, L"XInputGetState");
}

extern "C" DWORD WINAPI XInputGetStateEx(DWORD userIndex, XINPUT_STATE* state) {
    return ReadState(userIndex, state, true, L"XInputGetStateEx");
}

extern "C" DWORD WINAPI XInputSetState(DWORD userIndex, XINPUT_VIBRATION* vibration) JOYSHIM_XINPUT_SDK_NOEXCEPT {
    if (vibration == nullptr || userIndex >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (!RuntimeReadyOrQueue()) return ERROR_DEVICE_NOT_CONNECTED;
    LogXInputEntryOnce(L"XInputSetState");

    PublishedSlot published{};
    if (!CopyPublishedSlot(userIndex, published)) return ERROR_DEVICE_NOT_CONNECTED;
    if (!published.rumbleSupported) return ERROR_NOT_SUPPORTED;

    AcquireSRWLockExclusive(&g_controlLock);
    RumbleCommand& command = g_rumbleCommands[userIndex];
    command.left = vibration->wLeftMotorSpeed;
    command.right = vibration->wRightMotorSpeed;
    ++command.generation;
    ReleaseSRWLockExclusive(&g_controlLock);
    if (g_inputWakeEvent != nullptr) SetEvent(g_inputWakeEvent);
    return ERROR_SUCCESS;
}

extern "C" DWORD WINAPI XInputGetCapabilities(
    DWORD userIndex,
    DWORD flags,
    XINPUT_CAPABILITIES* capabilities) JOYSHIM_XINPUT_SDK_NOEXCEPT {
    if (capabilities == nullptr || userIndex >= XUSER_MAX_COUNT ||
        (flags != 0 && flags != XINPUT_FLAG_GAMEPAD)) return ERROR_BAD_ARGUMENTS;
    std::memset(capabilities, 0, sizeof(*capabilities));
    if (!RuntimeReadyOrQueue()) return ERROR_DEVICE_NOT_CONNECTED;
    LogXInputEntryOnce(L"XInputGetCapabilities");

    PublishedSlot published{};
    if (!CopyPublishedSlot(userIndex, published)) return ERROR_DEVICE_NOT_CONNECTED;
    WORD supportedButtons = published.supportedButtons;
    if (!ExposeGuideInGetState()) {
        supportedButtons &= static_cast<WORD>(~joyshim::Guide);
    }

    const auto hasHalf = [](const joyshim::HalfAxisSource& source) noexcept {
        return source.kind != joyshim::AxisSourceKind::None;
    };
    const auto hasSigned = [](const joyshim::SignedAxisSource& source) noexcept {
        return source.kind != joyshim::AxisSourceKind::None;
    };
    const joyshim::AxisRouting& axes = published.axisRouting;
    capabilities->Type = XINPUT_DEVTYPE_GAMEPAD;
    capabilities->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    capabilities->Flags = published.rumbleSupported ? XINPUT_CAPS_FFB_SUPPORTED : 0;
    capabilities->Gamepad.wButtons = supportedButtons;
    capabilities->Gamepad.bLeftTrigger = hasHalf(axes.leftTrigger) ? 0xFF : 0;
    capabilities->Gamepad.bRightTrigger = hasHalf(axes.rightTrigger) ? 0xFF : 0;
    capabilities->Gamepad.sThumbLX =
        (hasSigned(axes.leftStickX) || hasHalf(axes.leftStickLeft) ||
            hasHalf(axes.leftStickRight)) ? 0x7FFF : 0;
    capabilities->Gamepad.sThumbLY =
        (hasSigned(axes.leftStickY) || hasHalf(axes.leftStickUp) ||
            hasHalf(axes.leftStickDown)) ? 0x7FFF : 0;
    capabilities->Gamepad.sThumbRX =
        (hasSigned(axes.rightStickX) || hasHalf(axes.rightStickLeft) ||
            hasHalf(axes.rightStickRight)) ? 0x7FFF : 0;
    capabilities->Gamepad.sThumbRY =
        (hasSigned(axes.rightStickY) || hasHalf(axes.rightStickUp) ||
            hasHalf(axes.rightStickDown)) ? 0x7FFF : 0;
    capabilities->Vibration.wLeftMotorSpeed = published.rumbleSupported ? 0xFFFF : 0;
    capabilities->Vibration.wRightMotorSpeed = published.rumbleSupported ? 0xFFFF : 0;
    return ERROR_SUCCESS;
}

// 系统头文件可能提供 XInputGetCapabilitiesEx 的不同参数结构定义。
// 使用私有函数名避免 C 符号冲突，导出表映射为原生名称和序号。
extern "C" DWORD WINAPI JoyShimGetCapabilitiesEx(
    DWORD reserved,
    DWORD userIndex,
    DWORD flags,
    JoyShimXInputCapabilitiesEx* capabilities) {
    (void)reserved;
    if (capabilities == nullptr) return ERROR_BAD_ARGUMENTS;
    // 该结构含默认成员初始化；使用值初始化赋值清零，避免 GCC 的
    // -Wclass-memaccess 告警在 -Werror 模式下中断 MinGW 编译。
    *capabilities = JoyShimXInputCapabilitiesEx{};

    const DWORD result = XInputGetCapabilities(
        userIndex, flags, &capabilities->Capabilities);
    if (result != ERROR_SUCCESS) return result;

    PublishedSlot published{};
    if (CopyPublishedSlot(userIndex, published)) {
        capabilities->VendorId = published.identity.vendor;
        capabilities->ProductId = published.identity.product;
    }
    return ERROR_SUCCESS;
}

extern "C" void WINAPI XInputEnable(BOOL enable) JOYSHIM_XINPUT_SDK_NOEXCEPT {
    InterlockedExchange(&g_enabled, enable ? TRUE : FALSE);
    if (!enable) {
        AcquireSRWLockExclusive(&g_controlLock);
        for (RumbleCommand& command : g_rumbleCommands) {
            command.left = 0;
            command.right = 0;
            ++command.generation;
        }
        ReleaseSRWLockExclusive(&g_controlLock);
    }
    if (g_inputWakeEvent != nullptr) SetEvent(g_inputWakeEvent);
}

extern "C" DWORD WINAPI XInputGetDSoundAudioDeviceGuids(
    DWORD userIndex, GUID* renderGuid, GUID* captureGuid) {
    if (renderGuid == nullptr || captureGuid == nullptr) return ERROR_BAD_ARGUMENTS;
    std::memset(renderGuid, 0, sizeof(*renderGuid));
    std::memset(captureGuid, 0, sizeof(*captureGuid));
    return CheckConnected(userIndex) == ERROR_SUCCESS
        ? ERROR_NOT_SUPPORTED : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI XInputGetAudioDeviceIds(
    DWORD userIndex, LPWSTR renderId, UINT* renderCount,
    LPWSTR captureId, UINT* captureCount) JOYSHIM_XINPUT_SDK_NOEXCEPT {
    if (renderCount == nullptr || captureCount == nullptr ||
        userIndex >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (renderId != nullptr && *renderCount > 0) renderId[0] = L'\0';
    if (captureId != nullptr && *captureCount > 0) captureId[0] = L'\0';
    *renderCount = 0;
    *captureCount = 0;
    return CheckConnected(userIndex) == ERROR_SUCCESS
        ? ERROR_NOT_SUPPORTED : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI XInputGetBatteryInformation(
    DWORD userIndex, BYTE, XINPUT_BATTERY_INFORMATION* battery) JOYSHIM_XINPUT_SDK_NOEXCEPT {
    if (battery == nullptr || userIndex >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    std::memset(battery, 0, sizeof(*battery));
    return CheckConnected(userIndex) == ERROR_SUCCESS
        ? ERROR_NOT_SUPPORTED : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI XInputGetKeystroke(
    DWORD userIndex, DWORD, XINPUT_KEYSTROKE* keystroke) JOYSHIM_XINPUT_SDK_NOEXCEPT {
    if (keystroke == nullptr || userIndex >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    std::memset(keystroke, 0, sizeof(*keystroke));
    return CheckConnected(userIndex) == ERROR_SUCCESS
        ? ERROR_EMPTY : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI XInputWaitForGuideButton(DWORD, DWORD, void*) {
    return ERROR_NOT_SUPPORTED;
}
extern "C" DWORD WINAPI XInputCancelGuideButtonWait(DWORD) {
    return ERROR_NOT_SUPPORTED;
}
extern "C" DWORD WINAPI XInputPowerOffController(DWORD) {
    return ERROR_NOT_SUPPORTED;
}

// 官方 XInput 1.3 的序号 1 使用外部名称 DllMain。MSVC 19.51 的 x86
// .def 解析无法可靠导出这些 stdcall 项，因此 CMake 使用完整 /EXPORT 列表，
// 将本包装函数导出为 DllMain @1；真正的加载器入口点仍只转发初始化逻辑。
#ifdef BSCN_SINGLE_DLL
// 单个 dinput8.dll 同时承载输入代理及图标 Hook。入口不再重复定义 DllMain。
extern "C" int WINAPI BondOneReadControllerInfo(
    unsigned int slot, int* type, char* typeName, unsigned int typeNameCapacity,
    char* name, unsigned int nameCapacity, unsigned short* vendor,
    unsigned int* deviceId) {
    if (slot >= g_publishedSlots.size()) return 0;
    AcquireSRWLockShared(&g_stateLock);
    const auto& published = g_publishedSlots[slot];
    const bool connected = published.connected;
    if (connected) {
        if (type) *type = published.sdlType;
        if (vendor) *vendor = published.identity.vendor;
        if (deviceId) *deviceId = published.deviceId;
        if (typeName && typeNameCapacity) {
            const size_t size = std::strlen(published.sdlTypeName);
            const size_t count = size < typeNameCapacity - 1 ? size : typeNameCapacity - 1;
            std::memcpy(typeName, published.sdlTypeName, count);
            typeName[count] = '\0';
        }
        if (name && nameCapacity) {
            const size_t size = std::strlen(published.identity.name);
            const size_t count = size < nameCapacity - 1 ? size : nameCapacity - 1;
            std::memcpy(name, published.identity.name, count);
            name[count] = '\0';
        }
    }
    ReleaseSRWLockShared(&g_stateLock);
    return connected ? 1 : 0;
}
#endif

extern "C" BOOL WINAPI JoyShimExportedDllMain(
    HINSTANCE module, DWORD reason, LPVOID reserved) noexcept {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        // 在加载器锁内仅记录模块信息，不修改 IAT，不解析配置，不加载 SDL3，
        // 也不等待新线程。IAT 接管与启动由 loader lock 外的工作线程完成。
        joyshim::ProxyEntryProcessAttach(module);
#ifdef BSCN_SINGLE_DLL
        bondone::OpenSessionLog(module);
        BondIconsProcessAttach(module);
#endif
#ifndef JOYSHIM_LAZY_BOOTSTRAP_ONLY
        // 标准构建只排队一个不等待的线程，耗时操作由后台线程处理。
        joyshim::ProxyEntryQueueRuntimeBootstrap();
#endif
        // 惰性诊断构建：连线程创建都不进入 DllMain，首次真实代理
        // XInput / 系统 API 调用才排队启动；用于隔离启动崩溃成因。
    } else if (reason == DLL_PROCESS_DETACH) {
        // 不在 DllMain 中等待线程，只发送退出信号；所有 SDL 对象仍由输入线程关闭。
#ifdef BSCN_SINGLE_DLL
        BondIconsProcessDetach();
#endif
        joyshim::ProxyEntryProcessDetach();
        if (g_hotReloadStopEvent != nullptr) SetEvent(g_hotReloadStopEvent);
        if (g_inputStopEvent != nullptr) SetEvent(g_inputStopEvent);
        if (g_inputWakeEvent != nullptr) SetEvent(g_inputWakeEvent);
#ifdef BSCN_SINGLE_DLL
        bondone::CloseSessionLog();
#endif
    }
    return TRUE;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) noexcept {
    return JoyShimExportedDllMain(module, reason, reserved);
}
