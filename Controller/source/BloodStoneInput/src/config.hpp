#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "controller_match.hpp"
#include "device_correction.hpp"
#include "input_routing.hpp"

namespace joyshim {

constexpr std::size_t MaxControllerProfiles = 32;
constexpr std::size_t MaxProcessNames = 32;
constexpr std::size_t MaxProfileAlias = 96;

struct AxisCorrectionPatch {
    bool swapConfigured{};
    bool invertXConfigured{};
    bool invertYConfigured{};
    AxisCorrection value{};
};

struct AxisOutputPatch {
    AxisCorrectionPatch correction{};
    bool matrixXXConfigured{};
    bool matrixXYConfigured{};
    bool matrixYXConfigured{};
    bool matrixYYConfigured{};
    bool diagonalEnabledConfigured{};
    bool diagonalToleranceConfigured{};
    bool diagonalOffsetConfigured{};
    bool diagonalMinimumMagnitudeConfigured{};
    bool diagonalMinimumGapConfigured{};
    bool diagonalAxisConfigured{};
    bool diagonalSignModeConfigured{};
    bool separationModeConfigured{};
    bool separationMinimumMagnitudeConfigured{};
    bool separationSecondaryPercentConfigured{};
    bool separationSignModeConfigured{};
    AxisOutputTuning value{};
};

struct NativeInputPatch {
    bool blockDirectInputConfigured{};
    bool blockDirectInput{};
    bool traceDirectInputConfigured{};
    bool traceDirectInput{};
};

struct NativeInputPolicy {
    bool blockDirectInput{false};
    bool traceDirectInput{};
};

struct RoutingPatch {
    std::array<bool, XInputButtonTargetCount> buttonConfigured{};
    std::array<ButtonBinding, XInputButtonTargetCount> buttons{};
    std::array<bool, 4> signedConfigured{};
    std::array<SignedAxisSource, 4> signedAxes{};
    std::array<bool, 10> halfConfigured{};
    std::array<HalfAxisSource, 10> halfAxes{};
    std::uint32_t invalidEntries{};
};

struct ProfilePatch {
    AxisCorrectionPatch inputLeft{};
    AxisCorrectionPatch inputRight{};
    AxisOutputPatch outputLeft{};
    AxisOutputPatch outputRight{};
    RoutingPatch routing{};
    NativeInputPatch nativeInput{};
};

struct DeviceProfile {
    ControllerMatchKind controllerMatch{ControllerMatchKind::None};
    DeviceCorrection correction{};
    DeviceCorrection outputCorrection{};
    DeviceOutputTuning outputTuning{};
    InputRouting routing{};
    NativeInputPolicy nativeInput{};
    // 主项目设置始终作为当前游戏所有手柄的父集合默认值；匹配到的
    // Controllers.<自定义名称> 只覆盖自己明确配置的字段。
    bool projectPatchApplied{};
    bool controllerPatchApplied{};
    char controllerAlias[MaxProfileAlias]{};
};

// Controllers.<自定义名称> 的名称仅用于组织和日志，不参与设备匹配。
// 顶层字段声明这个手柄子集合；其 Buttons/Axes/NativeInput/Input/Output
// 子表声明只作用于该子集合的覆盖项。
struct ControllerProfileConfig : ControllerMatchFields {
    char alias[MaxProfileAlias]{};
    std::int32_t priority{};
    ProfilePatch patch{};
    std::uint32_t order{};
};

struct ProcessNameValue {
    wchar_t value[MAX_PATH]{};
};

// DLL/Mod 只服务当前游戏。[Project].ProcessNames 仅用于同一游戏含多个
// EXE 时，让其他 EXE 找到同一份 <主EXE>.joyshim.toml；精确同名配置可
// 省略该表。它不建立第二套“游戏配置层”。
struct ProjectConfig {
    std::vector<ProcessNameValue> processNames{};
    ProfilePatch patch{};
};

enum class ConfigSourceKind {
    GeneratedGeneric,
    Generic,
    ExactProcessProfile,
    SecondaryProcessProfile,
};

struct RuntimeConfig {
    wchar_t tomlPath[MAX_PATH]{};
    wchar_t genericTomlPath[MAX_PATH]{};
    wchar_t exactTomlPath[MAX_PATH]{};
    wchar_t selectedProfileOwner[MAX_PATH]{};
    wchar_t logPath[MAX_PATH]{};
    wchar_t mappingDatabasePath[MAX_PATH]{};
    wchar_t errorReportPath[MAX_PATH]{};
    wchar_t processName[MAX_PATH]{};
    ConfigSourceKind configSource{ConfigSourceKind::Generic};
    bool specializedConfig{};
    bool genericConfigIgnored{};
    // 主配置 [Input].Rumble 控制 SDL 手柄振动；默认开启。
    // 关闭时既不能向游戏报告 FFB 支持，也必须停止已发送的振动。
    bool enableRumble{true};
    bool enableLog{};
    bool verboseDeviceInfo{};
    bool enableExternalDatabase{true};
    bool exposeGuideInGetState{};
    bool enableHotReload{};
    bool reloadMappingDatabase{};
    std::uint32_t logStartupDelayMs{1000};
    std::uint32_t inputPollIntervalMs{1};
    std::uint32_t hotReloadStartupDelayMs{2000};
    std::uint32_t hotReloadPollMs{250};
    std::uint32_t hotReloadDebounceMs{300};

    // TOML 已存在时永不写回。错误报告写入独立的 .error.txt，绝不修改
    // 用户 TOML。热重载失败时由调用方继续保留上一份有效配置。
    bool configValid{true};
    bool configGenerated{};
    bool configReadFailed{};
    std::uint32_t parseErrorLine{};
    std::uint32_t parseErrorColumn{};
    wchar_t parseError[512]{};
    wchar_t parseErrorPath[384]{};
    wchar_t parseErrorExpected[384]{};
    wchar_t parseErrorActual[384]{};
    wchar_t parseErrorSuggestion[768]{};
    wchar_t parseErrorSource[768]{};

    ProjectConfig project{};
    std::vector<ControllerProfileConfig> controllers{};
};


struct DirectInputPolicyDecision {
    ControllerMatchKind controllerMatch{ControllerMatchKind::None};
    bool blockDirectInput{false};
    bool traceDirectInput{};
    bool projectPatchApplied{};
    bool controllerPatchApplied{};
    char controllerAlias[MaxProfileAlias]{};
};

// 配置绑定实际加载的代理 DLL 主名。以 winmm.dll 为例：优先查找
// <EXE主名>.winmm.toml；若不存在，只扫描 *.winmm.toml 中明确
// 声明 ProcessNames 的其他专版；否则使用同目录 winmm.toml。
// 每种 DLL 的配置互相隔离，专版与默认配置互斥，不进行合并。
RuntimeConfig LoadRuntimeConfig(HMODULE module) noexcept;

// 配置检查工具可显式验证七种代理的同名 TOML 和专版 TOML。
// 专版为 <EXE主名>.<代理DLL主名>.toml，以文件名推导 EXE 归属。
RuntimeConfig LoadRuntimeConfigFile(HMODULE module, const wchar_t* path) noexcept;

// 热重载固定重新读取启动时已经选中的文件，不在运行中因新增/删除配置而
// 悄然切换 Profile。失败时由调用方继续保留上一份有效配置。
RuntimeConfig ReloadRuntimeConfig(
    HMODULE module,
    const RuntimeConfig& current) noexcept;

// 合成顺序：内部默认值 -> 当前项目普通设置 -> 匹配到的手柄子集设置。
// 手柄子集只覆盖自己明确填写的项目，未填写项目继续继承项目默认值。
DeviceProfile LoadDeviceProfile(
    const RuntimeConfig& config,
    const DeviceIdentity& identity) noexcept;

// DirectInput 枚举发生在 SDL 打开设备之前，因此只使用 DirectInput 可见的
// VID/PID 与名称匹配手柄子集；原生输入策略同样按项目默认值 -> 手柄覆盖合成。
DirectInputPolicyDecision ResolveDirectInputPolicy(
    const RuntimeConfig& config,
    const DirectInputDeviceIdentity& identity) noexcept;

// 判断当前项目或任一已启用手柄子集合是否真正请求 DirectInput 包装。
// 只有 HideDirectInput（旧 BlockDirectInput）/TraceDirectInput 中至少一个有效值为 true 时才需要
// 建立 COM 代理；完全未启用时应透明返回系统原始接口。
bool ConfigRequestsDirectInputInterception(
    const RuntimeConfig& config) noexcept;

void WriteLogLine(const RuntimeConfig& config, const wchar_t* text) noexcept;

}  // namespace joyshim
