#include "config.hpp"
#ifdef BSCN_SINGLE_DLL
#include "bond_one_log.hpp"
#include "bloodstone_device_family.hpp"
#endif

#include <array>
#include <cctype>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "default_config.hpp"
#include "toml_reader.hpp"

namespace joyshim {
namespace {

constexpr DWORD MaxTomlBytes = 2u * 1024u * 1024u;

struct AsyncLogItem {
    wchar_t path[MAX_PATH]{};
    char utf8[4096]{};
    DWORD bytes{};
    std::uint32_t startupDelayMs{};
};

SRWLOCK g_logFileLock = SRWLOCK_INIT;
alignas(8) volatile LONG64 g_logNotBeforeTick{};

bool BuildSiblingPath(
    HMODULE module,
    const wchar_t* fileName,
    wchar_t* output,
    std::size_t outputCount) noexcept {
    if (output == nullptr || outputCount == 0 || fileName == nullptr) return false;
    const DWORD length = GetModuleFileNameW(module, output, static_cast<DWORD>(outputCount));
    if (length == 0 || length >= outputCount) {
        output[0] = L'\0';
        return false;
    }
    wchar_t* separator = std::wcsrchr(output, L'\\');
    if (separator == nullptr) separator = std::wcsrchr(output, L'/');
    if (separator == nullptr) {
        output[0] = L'\0';
        return false;
    }
    ++separator;
    const std::size_t prefixLength = static_cast<std::size_t>(separator - output);
    const std::size_t fileLength = std::wcslen(fileName);
    if (prefixLength + fileLength + 1 > outputCount) {
        output[0] = L'\0';
        return false;
    }
    std::wmemcpy(separator, fileName, fileLength + 1);
    return true;
}

// 从实际已加载模块的文件名得到配置主名，避免绑定某一种代理名称。
// 例如 winmm.dll 对应 winmm，xinput1_3.dll 对应 xinput1_3。
// 不依赖进程工作目录，也不需要游戏 EXE 的名称与代理名称相同。
bool BuildModuleConfigStem(
    HMODULE module,
    wchar_t* output,
    std::size_t outputCount) noexcept {
    if (output == nullptr || outputCount == 0) return false;
    output[0] = L'\0';
    wchar_t modulePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(module, modulePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    const wchar_t* baseName = std::wcsrchr(modulePath, L'\\');
    baseName = baseName != nullptr ? baseName + 1 : modulePath;
    const wchar_t* extension = std::wcsrchr(baseName, L'.');
    if (extension == nullptr || extension == baseName) return false;
    const std::size_t stemLength = static_cast<std::size_t>(extension - baseName);
    if (stemLength + 1 > outputCount) return false;
    std::wmemcpy(output, baseName, stemLength);
    output[stemLength] = L'\0';
    return true;
}

// 游戏级专版以当前代理的主名结束。例如 Game.winmm.toml 只服务
// winmm.dll，避免同一目录内的多种代理读取彼此的配置。
bool BuildProxyProfileSuffix(
    const wchar_t* moduleStem,
    wchar_t* output,
    std::size_t outputCount) noexcept {
    if (moduleStem == nullptr || output == nullptr || outputCount == 0) return false;
    const int written = std::swprintf(output, outputCount, L".%ls.toml", moduleStem);
    return written > 0 && static_cast<std::size_t>(written) < outputCount;
}

bool IsAbsolutePath(const wchar_t* path) noexcept {
    if (path == nullptr || path[0] == L'\0') return false;
    if ((path[0] == L'\\' && path[1] == L'\\') ||
        (path[0] == L'/' && path[1] == L'/')) return true;
    return path[1] == L':';
}

bool ResolveConfiguredPath(
    HMODULE module,
    const wchar_t* configuredPath,
    wchar_t* output,
    std::size_t outputCount) noexcept {
    if (configuredPath == nullptr || configuredPath[0] == L'\0') {
        if (output != nullptr && outputCount > 0) output[0] = L'\0';
        return false;
    }
    if (!IsAbsolutePath(configuredPath)) {
        return BuildSiblingPath(module, configuredPath, output, outputCount);
    }
    const std::size_t length = std::wcslen(configuredPath);
    if (output == nullptr || length + 1 > outputCount) return false;
    std::wmemcpy(output, configuredPath, length + 1);
    return true;
}

void CopyProcessName(wchar_t* output, std::size_t count) noexcept {
    if (output == nullptr || count == 0) return;
    output[0] = L'\0';
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return;
    const wchar_t* name = std::wcsrchr(path, L'\\');
    name = name != nullptr ? name + 1 : path;
    const std::size_t sourceLength = std::wcslen(name);
    const std::size_t copyLength = sourceLength < count - 1 ? sourceLength : count - 1;
    std::wmemcpy(output, name, copyLength);
    output[copyLength] = L'\0';
}

bool EqualsWideIgnoreCase(const wchar_t* left, const wchar_t* right) noexcept {
    if (left == nullptr || right == nullptr) return false;
    while (*left != L'\0' && *right != L'\0') {
        if (std::towlower(*left) != std::towlower(*right)) return false;
        ++left;
        ++right;
    }
    return *left == *right;
}

bool EqualsAsciiIgnoreCase(const char* left, const char* right) noexcept {
    if (left == nullptr || right == nullptr) return false;
    while (*left != '\0' && *right != '\0') {
        const unsigned char l = static_cast<unsigned char>(*left);
        const unsigned char r = static_cast<unsigned char>(*right);
        if (std::tolower(l) != std::tolower(r)) return false;
        ++left;
        ++right;
    }
    return *left == *right;
}

bool CopyUtf8(const std::string& source, char* output, std::size_t outputCount) noexcept {
    if (output == nullptr || outputCount == 0 || source.size() >= outputCount) return false;
    std::memcpy(output, source.data(), source.size());
    output[source.size()] = '\0';
    return true;
}

bool Utf8ToWide(const std::string& source, wchar_t* output, int outputCount) noexcept {
    if (output == nullptr || outputCount <= 0) return false;
    output[0] = L'\0';
    if (source.empty()) return true;
    return MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, source.c_str(), -1, output, outputCount) != 0;
}

void CopyWideText(const wchar_t* source, wchar_t* output, std::size_t count) noexcept {
    if (output == nullptr || count == 0) return;
    output[0] = L'\0';
    if (source == nullptr) return;
    const std::size_t length = std::wcslen(source);
    const std::size_t copyLength = length < count - 1 ? length : count - 1;
    std::wmemcpy(output, source, copyLength);
    output[copyLength] = L'\0';
}

void CopyUtf8Text(const std::string& source, wchar_t* output, std::size_t count) noexcept {
    if (output == nullptr || count == 0) return;
    output[0] = L'\0';
    if (source.empty()) return;
    MultiByteToWideChar(
        CP_UTF8,
        0,
        source.c_str(),
        -1,
        output,
        static_cast<int>(count));
    output[count - 1] = L'\0';
}

void SetConfigError(
    RuntimeConfig& config,
    std::size_t line,
    std::size_t column,
    const wchar_t* path,
    const wchar_t* message,
    const wchar_t* expected,
    const wchar_t* actual,
    const wchar_t* suggestion,
    const wchar_t* source) noexcept {
    config.configValid = false;
    config.parseErrorLine = static_cast<std::uint32_t>(line);
    config.parseErrorColumn = static_cast<std::uint32_t>(column);
    CopyWideText(message != nullptr ? message : L"未知配置错误",
                 config.parseError, std::size(config.parseError));
    CopyWideText(path, config.parseErrorPath, std::size(config.parseErrorPath));
    CopyWideText(expected, config.parseErrorExpected, std::size(config.parseErrorExpected));
    CopyWideText(actual, config.parseErrorActual, std::size(config.parseErrorActual));
    CopyWideText(suggestion, config.parseErrorSuggestion, std::size(config.parseErrorSuggestion));
    CopyWideText(source, config.parseErrorSource, std::size(config.parseErrorSource));
}

void SetEntryError(
    RuntimeConfig& config,
    const toml::Entry& entry,
    const wchar_t* message,
    const wchar_t* expected,
    const wchar_t* actual,
    const wchar_t* suggestion) noexcept {
    wchar_t path[384]{};
    wchar_t source[768]{};
    CopyUtf8Text(toml::JoinPath(entry.table, entry.key), path, std::size(path));
    CopyUtf8Text(entry.sourceLine, source, std::size(source));
    SetConfigError(
        config,
        entry.line,
        entry.column,
        path,
        message,
        expected,
        actual,
        suggestion,
        source);
}

void SetParseError(RuntimeConfig& config, const toml::ParseError& error) noexcept {
    wchar_t message[512]{};
    wchar_t suggestion[768]{};
    wchar_t source[768]{};
    CopyUtf8Text(error.message, message, std::size(message));
    CopyUtf8Text(error.suggestion, suggestion, std::size(suggestion));
    CopyUtf8Text(error.sourceLine, source, std::size(source));
    SetConfigError(
        config,
        error.line,
        error.column,
        L"TOML 语法",
        message[0] != L'\0' ? message : L"TOML 解析错误",
        L"符合 JoyShim 严格 TOML 语法",
        L"语法无效",
        suggestion,
        source);
}

const wchar_t* ValueTypeNameWide(toml::ValueType type) noexcept {
    switch (type) {
        case toml::ValueType::Boolean: return L"布尔值";
        case toml::ValueType::Integer: return L"整数";
        case toml::ValueType::Floating: return L"浮点数";
        case toml::ValueType::String: return L"字符串";
        case toml::ValueType::StringArray: return L"字符串数组";
        default: return L"未知类型";
    }
}

bool ExpectBoolean(const toml::Entry& entry, bool& output, RuntimeConfig& config) noexcept {
    if (entry.value.type != toml::ValueType::Boolean) {
        SetEntryError(
            config, entry,
            L"字段类型错误。",
            L"布尔值 true 或 false",
            ValueTypeNameWide(entry.value.type),
            L"删除引号并使用小写形式，例如 Enabled = true。");
        return false;
    }
    output = entry.value.boolean;
    return true;
}

bool ExpectInteger(
    const toml::Entry& entry,
    std::int64_t minimum,
    std::int64_t maximum,
    std::int64_t& output,
    RuntimeConfig& config) noexcept {
    if (entry.value.type != toml::ValueType::Integer) {
        SetEntryError(
            config, entry,
            L"字段类型错误。",
            L"整数",
            ValueTypeNameWide(entry.value.type),
            L"整数不要加引号；十六进制可写成 0x1234。");
        return false;
    }
    if (entry.value.integer < minimum || entry.value.integer > maximum) {
        wchar_t expected[192]{};
        wchar_t actual[96]{};
        std::swprintf(expected, std::size(expected), L"范围 %lld～%lld 的整数",
                      static_cast<long long>(minimum), static_cast<long long>(maximum));
        std::swprintf(actual, std::size(actual), L"%lld",
                      static_cast<long long>(entry.value.integer));
        SetEntryError(
            config, entry,
            L"整数超出允许范围。",
            expected,
            actual,
            L"将该值调整到允许范围内；默认值可参考随代理 DLL 提供的同名 TOML。");
        return false;
    }
    output = entry.value.integer;
    return true;
}

bool ExpectString(const toml::Entry& entry, const std::string*& output, RuntimeConfig& config) noexcept {
    if (entry.value.type != toml::ValueType::String) {
        SetEntryError(
            config, entry,
            L"字段类型错误。",
            L"TOML 字符串",
            ValueTypeNameWide(entry.value.type),
            L"使用双引号或单引号，例如 Controller = \"MyPad\"。");
        return false;
    }
    output = &entry.value.string;
    return true;
}

bool IsValidProcessFileName(const wchar_t* value) noexcept {
    if (value == nullptr || value[0] == L'\0') return false;
    if (std::wcschr(value, L'\\') != nullptr || std::wcschr(value, L'/') != nullptr) return false;
    if (std::wcschr(value, L'*') != nullptr || std::wcschr(value, L'?') != nullptr) return false;
    const std::size_t length = std::wcslen(value);
    if (length <= 4) return false;
    constexpr wchar_t extension[] = L".exe";
    for (std::size_t index = 0; index < 4; ++index) {
        if (std::towlower(value[length - 4 + index]) != std::towlower(extension[index])) return false;
    }
    return true;
}

bool ParseProcessNames(
    ProjectConfig& project,
    RuntimeConfig& config,
    const toml::Entry& entry) noexcept {
    if (entry.value.type != toml::ValueType::StringArray) {
        SetEntryError(
            config, entry,
            L"ProcessNames 字段类型错误。",
            L"完整 EXE 文件名字符串数组，例如 ProcessNames = [\"Game.exe\"]",
            ValueTypeNameWide(entry.value.type),
            L"即使只有一个进程，也必须使用字符串数组；不要省略 .exe。");
        return false;
    }
    if (entry.value.strings.empty() || entry.value.strings.size() > MaxProcessNames) {
        SetEntryError(
            config, entry,
            L"ProcessNames 数组数量无效。",
            L"1～32 个完整 EXE 文件名",
            L"空数组或数量超限",
            L"同一游戏的多个 EXE 全部写入同一个数组，数组内部为 OR。");
        return false;
    }
    project.processNames.clear();
    for (const std::string& source : entry.value.strings) {
        ProcessNameValue name{};
        if (!Utf8ToWide(source, name.value, MAX_PATH) || !IsValidProcessFileName(name.value)) {
            SetEntryError(
                config, entry,
                L"ProcessNames 包含无效进程文件名。",
                L"不含路径、通配符且以 .exe 结尾的完整文件名",
                L"编码无效、缺少 .exe、包含路径或通配符",
                L"直接复制资源管理器中的 EXE 文件名，例如 \"Game.exe\"。");
            return false;
        }
        bool duplicate = false;
        for (const ProcessNameValue& existing : project.processNames) {
            if (EqualsWideIgnoreCase(existing.value, name.value)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            SetEntryError(
                config, entry,
                L"ProcessNames 包含重复项。",
                L"每个 EXE 文件名只出现一次",
                name.value,
                L"删除重复项；匹配不区分 ASCII 大小写。");
            return false;
        }
        project.processNames.push_back(name);
    }
    return true;
}

bool SetUnknownFieldError(
    RuntimeConfig& config,
    const toml::Entry& entry,
    const wchar_t* allowed) noexcept {
    SetEntryError(
        config,
        entry,
        L"未知字段或字段名拼写错误。",
        allowed,
        L"未定义字段",
        L"核对字段大小写和所属表；严格模式不会忽略未知字段。");
    return false;
}

bool SetUnknownTableError(
    RuntimeConfig& config,
    const toml::Entry& entry,
    const wchar_t* allowed) noexcept {
    SetEntryError(
        config,
        entry,
        L"未知或位置错误的 TOML 表。",
        allowed,
        L"不受支持的层级",
        L"层级节点名称区分大小写；Controllers.<自定义名称> 的名称可自由定义，但 Global、Project、Controllers 和功能子表必须准确。");
    return false;
}

bool IsHexGuid(const std::string& text) noexcept {
    if (text.size() != 32) return false;
    for (char value : text) {
        if (!std::isxdigit(static_cast<unsigned char>(value))) return false;
    }
    return true;
}

void NormalizeGuid(const std::string& source, char output[33]) noexcept {
    for (std::size_t index = 0; index < 32; ++index) {
        output[index] = static_cast<char>(std::tolower(static_cast<unsigned char>(source[index])));
    }
    output[32] = '\0';
}

DeviceProfile MakeDefaultDeviceProfile() noexcept {
    DeviceProfile profile{};
    profile.routing = MakeDefaultInputRouting();
    return profile;
}

void ApplyAxisCorrectionPatch(
    AxisCorrection& target,
    const AxisCorrectionPatch& patch) noexcept {
    if (patch.swapConfigured) target.swapXY = patch.value.swapXY;
    if (patch.invertXConfigured) target.invertX = patch.value.invertX;
    if (patch.invertYConfigured) target.invertY = patch.value.invertY;
}

void ApplyAxisOutputPatch(
    AxisCorrection& correction,
    AxisOutputTuning& tuning,
    const AxisOutputPatch& patch) noexcept {
    ApplyAxisCorrectionPatch(correction, patch.correction);
    if (patch.matrixXXConfigured) tuning.matrix.xx = patch.value.matrix.xx;
    if (patch.matrixXYConfigured) tuning.matrix.xy = patch.value.matrix.xy;
    if (patch.matrixYXConfigured) tuning.matrix.yx = patch.value.matrix.yx;
    if (patch.matrixYYConfigured) tuning.matrix.yy = patch.value.matrix.yy;
    if (patch.diagonalEnabledConfigured) tuning.diagonal.enabled = patch.value.diagonal.enabled;
    if (patch.diagonalToleranceConfigured) tuning.diagonal.tolerance = patch.value.diagonal.tolerance;
    if (patch.diagonalOffsetConfigured) tuning.diagonal.offset = patch.value.diagonal.offset;
    if (patch.diagonalMinimumMagnitudeConfigured) {
        tuning.diagonal.minimumMagnitude = patch.value.diagonal.minimumMagnitude;
    }
    if (patch.diagonalMinimumGapConfigured) {
        tuning.diagonal.minimumGap = patch.value.diagonal.minimumGap;
    }
    if (patch.diagonalAxisConfigured) tuning.diagonal.axis = patch.value.diagonal.axis;
    if (patch.diagonalSignModeConfigured) tuning.diagonal.signMode = patch.value.diagonal.signMode;
    if (patch.separationModeConfigured) {
        tuning.separation.mode = patch.value.separation.mode;
    }
    if (patch.separationMinimumMagnitudeConfigured) {
        tuning.separation.minimumMagnitude = patch.value.separation.minimumMagnitude;
    }
    if (patch.separationSecondaryPercentConfigured) {
        tuning.separation.secondaryPercent = patch.value.separation.secondaryPercent;
    }
    if (patch.separationSignModeConfigured) {
        tuning.separation.signMode = patch.value.separation.signMode;
    }
}

void ApplyRoutingPatch(InputRouting& routing, const RoutingPatch& patch) noexcept {
    for (std::size_t index = 0; index < patch.buttonConfigured.size(); ++index) {
        if (!patch.buttonConfigured[index]) continue;
        routing.buttons[index] = patch.buttons[index];
        routing.customized = true;
    }
    SignedAxisSource* signedTargets[] = {
        &routing.axes.leftStickX,
        &routing.axes.leftStickY,
        &routing.axes.rightStickX,
        &routing.axes.rightStickY,
    };
    for (std::size_t index = 0; index < patch.signedConfigured.size(); ++index) {
        if (!patch.signedConfigured[index]) continue;
        *signedTargets[index] = patch.signedAxes[index];
        routing.customized = true;
    }
    HalfAxisSource* halfTargets[] = {
        &routing.axes.leftStickLeft,
        &routing.axes.leftStickRight,
        &routing.axes.leftStickUp,
        &routing.axes.leftStickDown,
        &routing.axes.rightStickLeft,
        &routing.axes.rightStickRight,
        &routing.axes.rightStickUp,
        &routing.axes.rightStickDown,
        &routing.axes.leftTrigger,
        &routing.axes.rightTrigger,
    };
    constexpr std::size_t relatedSigned[] = {0, 0, 1, 1, 2, 2, 3, 3, 99, 99};
    for (std::size_t index = 0; index < patch.halfConfigured.size(); ++index) {
        if (!patch.halfConfigured[index]) continue;
        *halfTargets[index] = patch.halfAxes[index];
        if (relatedSigned[index] != 99 && !patch.signedConfigured[relatedSigned[index]]) {
            *signedTargets[relatedSigned[index]] = {};
        }
        routing.customized = true;
    }
    const std::uint32_t invalidTotal =
        static_cast<std::uint32_t>(routing.invalidEntries) + patch.invalidEntries;
    routing.invalidEntries = static_cast<std::uint16_t>(
        invalidTotal > 0xFFFFu ? 0xFFFFu : invalidTotal);
}

void ApplyNativeInputPatch(
    NativeInputPolicy& target,
    const NativeInputPatch& patch) noexcept {
    if (patch.blockDirectInputConfigured) {
        target.blockDirectInput = patch.blockDirectInput;
    }
    if (patch.traceDirectInputConfigured) {
        target.traceDirectInput = patch.traceDirectInput;
    }
}

void ApplyProfilePatch(DeviceProfile& profile, const ProfilePatch& patch) noexcept {
    ApplyAxisCorrectionPatch(profile.correction.leftStick, patch.inputLeft);
    ApplyAxisCorrectionPatch(profile.correction.rightStick, patch.inputRight);
    ApplyAxisOutputPatch(
        profile.outputCorrection.leftStick,
        profile.outputTuning.leftStick,
        patch.outputLeft);
    ApplyAxisOutputPatch(
        profile.outputCorrection.rightStick,
        profile.outputTuning.rightStick,
        patch.outputRight);
    ApplyRoutingPatch(profile.routing, patch.routing);
    ApplyNativeInputPatch(profile.nativeInput, patch.nativeInput);
}

bool HasProfilePatch(const ProfilePatch& patch) noexcept {
    if (patch.inputLeft.swapConfigured || patch.inputLeft.invertXConfigured ||
        patch.inputLeft.invertYConfigured || patch.inputRight.swapConfigured ||
        patch.inputRight.invertXConfigured || patch.inputRight.invertYConfigured ||
        patch.nativeInput.blockDirectInputConfigured ||
        patch.nativeInput.traceDirectInputConfigured || patch.routing.invalidEntries != 0) {
        return true;
    }
    const AxisOutputPatch* outputs[] = {&patch.outputLeft, &patch.outputRight};
    for (const AxisOutputPatch* output : outputs) {
        if (output->correction.swapConfigured || output->correction.invertXConfigured ||
            output->correction.invertYConfigured || output->matrixXXConfigured ||
            output->matrixXYConfigured || output->matrixYXConfigured ||
            output->matrixYYConfigured || output->diagonalEnabledConfigured ||
            output->diagonalToleranceConfigured || output->diagonalOffsetConfigured ||
            output->diagonalMinimumMagnitudeConfigured ||
            output->diagonalMinimumGapConfigured || output->diagonalAxisConfigured ||
            output->diagonalSignModeConfigured || output->separationModeConfigured ||
            output->separationMinimumMagnitudeConfigured ||
            output->separationSecondaryPercentConfigured ||
            output->separationSignModeConfigured) {
            return true;
        }
    }
    for (bool configured : patch.routing.buttonConfigured) {
        if (configured) return true;
    }
    for (bool configured : patch.routing.signedConfigured) {
        if (configured) return true;
    }
    for (bool configured : patch.routing.halfConfigured) {
        if (configured) return true;
    }
    return false;
}

ControllerProfileConfig* FindOrCreateController(
    RuntimeConfig& config,
    const std::string& alias) noexcept {
    for (ControllerProfileConfig& controller : config.controllers) {
        if (alias == controller.alias) return &controller;
    }
    if (config.controllers.size() >= MaxControllerProfiles || alias.empty() ||
        alias.size() >= MaxProfileAlias) {
        return nullptr;
    }
    config.controllers.emplace_back();
    ControllerProfileConfig& controller = config.controllers.back();
    CopyUtf8(alias, controller.alias, std::size(controller.alias));
    controller.order = static_cast<std::uint32_t>(config.controllers.size() - 1);
    return &controller;
}

bool ParseConnection(
    const std::string& text,
    ConnectionKind& output) noexcept {
    if (EqualsAsciiIgnoreCase(text.c_str(), "Wired")) {
        output = ConnectionKind::Wired;
        return true;
    }
    if (EqualsAsciiIgnoreCase(text.c_str(), "Wireless")) {
        output = ConnectionKind::Wireless;
        return true;
    }
    if (EqualsAsciiIgnoreCase(text.c_str(), "Unknown")) {
        output = ConnectionKind::Unknown;
        return true;
    }
    return false;
}

bool ParseDeviceTextArray(
    std::vector<DeviceTextValue>& target,
    RuntimeConfig& config,
    const toml::Entry& entry,
    const wchar_t* description) noexcept {
    if (entry.value.type != toml::ValueType::StringArray) {
        SetEntryError(
            config, entry,
            L"设备匹配字段类型错误。",
            L"字符串数组",
            ValueTypeNameWide(entry.value.type),
            L"即使只有一个值，也使用数组；多个候选值按 OR 匹配。");
        return false;
    }
    if (entry.value.strings.empty() || entry.value.strings.size() > MaxControllerTextMatches) {
        SetEntryError(
            config, entry,
            L"设备匹配数组数量无效。",
            L"1～16 个非空字符串",
            L"空数组或数量超限",
            description);
        return false;
    }
    target.clear();
    for (const std::string& source : entry.value.strings) {
        DeviceTextValue value{};
        if (source.empty() || !CopyUtf8(source, value.value, std::size(value.value))) {
            SetEntryError(
                config, entry,
                L"设备匹配字符串无效。",
                L"少于 256 字节的非空 UTF-8 字符串",
                L"空值、编码无效或字符串过长",
                description);
            return false;
        }
        for (const DeviceTextValue& existing : target) {
            if (EqualsAsciiIgnoreCase(existing.value, value.value)) {
                SetEntryError(
                    config, entry,
                    L"设备匹配数组包含重复项。",
                    L"每个匹配值只出现一次",
                    L"忽略大小写后重复",
                    L"删除重复项；数组内部已经按 OR 匹配。");
                return false;
            }
        }
        target.push_back(value);
    }
    return true;
}

bool ParseDevicePathArray(
    std::vector<DevicePathValue>& target,
    RuntimeConfig& config,
    const toml::Entry& entry) noexcept {
    if (entry.value.type != toml::ValueType::StringArray) {
        SetEntryError(
            config, entry,
            L"Paths 字段类型错误。",
            L"字符串数组",
            ValueTypeNameWide(entry.value.type),
            L"即使只有一个设备路径，也使用数组；多个候选路径按 OR 匹配。");
        return false;
    }
    if (entry.value.strings.empty() || entry.value.strings.size() > MaxControllerTextMatches) {
        SetEntryError(
            config, entry,
            L"Paths 数组数量无效。",
            L"1～16 个非空设备路径",
            L"空数组或数量超限",
            L"设备路径只适合用户主动锁定具体设备；发行配置通常不应填写。");
        return false;
    }
    target.clear();
    for (const std::string& source : entry.value.strings) {
        DevicePathValue value{};
        if (source.empty() || !CopyUtf8(source, value.value, std::size(value.value))) {
            SetEntryError(
                config, entry,
                L"Paths 包含无效设备路径。",
                L"少于 512 字节的非空 UTF-8 字符串",
                L"空值、编码无效或字符串过长",
                L"运行 JoyShimGamepadInfo.exe 并复制完整设备路径。");
            return false;
        }
        for (const DevicePathValue& existing : target) {
            if (EqualsAsciiIgnoreCase(existing.value, value.value)) {
                SetEntryError(
                    config, entry,
                    L"Paths 包含重复项。",
                    L"每个设备路径只出现一次",
                    L"忽略大小写后重复",
                    L"删除重复项；数组内部已经按 OR 匹配。");
                return false;
            }
        }
        target.push_back(value);
    }
    return true;
}

int HexDigitValue(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return 10 + value - 'a';
    if (value >= 'A' && value <= 'F') return 10 + value - 'A';
    return -1;
}

bool ParseDeviceId(const std::string& text, DeviceIdValue& output) noexcept {
    if (text.size() != 9 || text[4] != ':') return false;
    unsigned int vendor = 0;
    unsigned int product = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        const int digit = HexDigitValue(text[index]);
        if (digit < 0) return false;
        vendor = (vendor << 4u) | static_cast<unsigned int>(digit);
    }
    for (std::size_t index = 5; index < 9; ++index) {
        const int digit = HexDigitValue(text[index]);
        if (digit < 0) return false;
        product = (product << 4u) | static_cast<unsigned int>(digit);
    }
    output.vendor = static_cast<std::uint16_t>(vendor);
    output.product = static_cast<std::uint16_t>(product);
    return true;
}

bool ParseDeviceIds(
    ControllerProfileConfig& controller,
    RuntimeConfig& config,
    const toml::Entry& entry) noexcept {
    if (entry.value.type != toml::ValueType::StringArray) {
        SetEntryError(
            config, entry,
            L"DeviceIds 字段类型错误。",
            L"字符串数组，例如 DeviceIds = [\"1234:5678\"]",
            ValueTypeNameWide(entry.value.type),
            L"VID/PID 必须成对写入，避免两个独立数组产生错误交叉组合。");
        return false;
    }
    if (entry.value.strings.empty() || entry.value.strings.size() > MaxControllerDeviceIds) {
        SetEntryError(
            config, entry,
            L"DeviceIds 数组数量无效。",
            L"1～32 个 VVVV:PPPP",
            L"空数组或数量超限",
            L"每个元素写成四位 VID、冒号、四位 PID，例如 1234:5678。");
        return false;
    }
    controller.deviceIds.clear();
    for (const std::string& source : entry.value.strings) {
        DeviceIdValue value{};
        if (!ParseDeviceId(source, value)) {
            SetEntryError(
                config, entry,
                L"DeviceIds 包含无效 VID:PID。",
                L"恰好 VVVV:PPPP，例如 1234:5678",
                L"格式或十六进制字符无效",
                L"不要拆成 VendorIds/ProductIds；VID 与 PID 必须保持成对关系。");
            return false;
        }
        for (const DeviceIdValue& existing : controller.deviceIds) {
            if (existing.vendor == value.vendor && existing.product == value.product) {
                SetEntryError(
                    config, entry,
                    L"DeviceIds 包含重复项。",
                    L"每个 VID:PID 只出现一次",
                    L"重复设备 ID",
                    L"删除重复项；数组内部按 OR 匹配。");
                return false;
            }
        }
        controller.deviceIds.push_back(value);
    }
    return true;
}

bool ParseConnections(
    ControllerProfileConfig& controller,
    RuntimeConfig& config,
    const toml::Entry& entry) noexcept {
    if (entry.value.type != toml::ValueType::StringArray) {
        SetEntryError(
            config, entry,
            L"Connections 字段类型错误。",
            L"字符串数组，例如 Connections = [\"Wired\", \"Wireless\"]",
            ValueTypeNameWide(entry.value.type),
            L"多个连接方式按 OR 匹配。");
        return false;
    }
    if (entry.value.strings.empty() || entry.value.strings.size() > 3) {
        SetEntryError(
            config, entry,
            L"Connections 数组数量无效。",
            L"1～3 个连接方式",
            L"空数组或数量超限",
            L"只允许 Wired、Wireless、Unknown。");
        return false;
    }
    controller.connections.clear();
    for (const std::string& source : entry.value.strings) {
        ConnectionKind value = ConnectionKind::Unknown;
        if (!ParseConnection(source, value)) {
            SetEntryError(
                config, entry,
                L"Connections 包含无效枚举值。",
                L"\"Wired\"、\"Wireless\" 或 \"Unknown\"",
                L"未知字符串",
                L"连接方式不稳定时可删除整个字段。");
            return false;
        }
        bool duplicate = false;
        for (ConnectionKind existing : controller.connections) {
            if (existing == value) duplicate = true;
        }
        if (duplicate) {
            SetEntryError(
                config, entry,
                L"Connections 包含重复项。",
                L"每种连接方式只出现一次",
                L"重复枚举值",
                L"删除重复项；数组内部按 OR 匹配。");
            return false;
        }
        controller.connections.push_back(value);
    }
    return true;
}

bool ParseGlobalEntry(
    HMODULE module,
    RuntimeConfig& config,
    const toml::Entry& entry) noexcept {
    const auto& path = entry.table;
    std::int64_t integer = 0;
    bool boolean = false;
    const std::string* string = nullptr;
    if (path.size() == 1 && path[0] == "Global") {
        if (entry.key == "InputPollIntervalMs") {
            if (!ExpectInteger(entry, 1, 16, integer, config)) return false;
            config.inputPollIntervalMs = static_cast<std::uint32_t>(integer);
        } else if (entry.key == "ExposeGuideInGetState") {
            if (!ExpectBoolean(entry, boolean, config)) return false;
            config.exposeGuideInGetState = boolean;
        } else {
            return SetUnknownFieldError(
                config, entry, L"Global 仅接受 InputPollIntervalMs、ExposeGuideInGetState");
        }
        return true;
    }
    if (path.size() == 2 && path[0] == "Global" && path[1] == "HotReload") {
        if (entry.key == "Enabled") {
            if (!ExpectBoolean(entry, boolean, config)) return false;
            config.enableHotReload = boolean;
        } else if (entry.key == "StartupDelayMs") {
            if (!ExpectInteger(entry, 0, 60000, integer, config)) return false;
            config.hotReloadStartupDelayMs = static_cast<std::uint32_t>(integer);
        } else if (entry.key == "PollIntervalMs") {
            if (!ExpectInteger(entry, 50, 60000, integer, config)) return false;
            config.hotReloadPollMs = static_cast<std::uint32_t>(integer);
        } else if (entry.key == "DebounceMs") {
            if (!ExpectInteger(entry, 0, 60000, integer, config)) return false;
            config.hotReloadDebounceMs = static_cast<std::uint32_t>(integer);
        } else if (entry.key == "ReloadMappingDatabase") {
            if (!ExpectBoolean(entry, boolean, config)) return false;
            config.reloadMappingDatabase = boolean;
        } else {
            return SetUnknownFieldError(
                config, entry,
                L"Global.HotReload 仅接受 Enabled、StartupDelayMs、PollIntervalMs、DebounceMs、ReloadMappingDatabase");
        }
        return true;
    }
    if (path.size() == 2 && path[0] == "Global" && path[1] == "Diagnostics") {
        if (entry.key == "Enabled") {
            if (!ExpectBoolean(entry, boolean, config)) return false;
            config.enableLog = boolean;
        } else if (entry.key == "VerboseDeviceInfo") {
            if (!ExpectBoolean(entry, boolean, config)) return false;
            config.verboseDeviceInfo = boolean;
        } else if (entry.key == "StartupDelayMs") {
            if (!ExpectInteger(entry, 0, 60000, integer, config)) return false;
            config.logStartupDelayMs = static_cast<std::uint32_t>(integer);
        } else {
            return SetUnknownFieldError(
                config, entry, L"Global.Diagnostics 仅接受 Enabled、VerboseDeviceInfo、StartupDelayMs");
        }
        return true;
    }
    if (path.size() == 2 && path[0] == "Global" && path[1] == "MappingDatabase") {
        if (entry.key == "Enabled") {
            if (!ExpectBoolean(entry, boolean, config)) return false;
            config.enableExternalDatabase = boolean;
        } else if (entry.key == "Path") {
            if (!ExpectString(entry, string, config)) return false;
            wchar_t configured[MAX_PATH]{};
            if (!Utf8ToWide(*string, configured, MAX_PATH) ||
                !ResolveConfiguredPath(module, configured, config.mappingDatabasePath, MAX_PATH)) {
                SetEntryError(
                    config, entry,
                    L"MappingDatabase.Path 无效或路径过长。",
                    L"有效的相对或绝对 Windows 路径",
                    L"无法解析的路径",
                    L"推荐使用同目录相对路径，例如 Path = \"gamecontrollerdb.txt\"。");
                return false;
            }
        } else {
            return SetUnknownFieldError(
                config, entry, L"Global.MappingDatabase 仅接受 Enabled、Path");
        }
        return true;
    }
    return SetUnknownTableError(
        config, entry,
        L"Global、Global.HotReload、Global.MappingDatabase、Global.Diagnostics");
}

bool ParseControllerRoot(
    ControllerProfileConfig& controller,
    RuntimeConfig& config,
    const toml::Entry& entry) noexcept {
    bool boolean = false;
    std::int64_t integer = 0;
    if (entry.key == "Enabled") {
        if (!ExpectBoolean(entry, boolean, config)) return false;
        controller.enabled = boolean;
    } else if (entry.key == "MatchAny") {
        if (!ExpectBoolean(entry, boolean, config)) return false;
        controller.matchAny = boolean;
    } else if (entry.key == "Priority") {
        if (!ExpectInteger(entry, -100000, 100000, integer, config)) return false;
        controller.priority = static_cast<std::int32_t>(integer);
    } else if (entry.key == "Guids") {
        if (entry.value.type != toml::ValueType::StringArray) {
            SetEntryError(
                config, entry,
                L"字段类型错误。",
                L"字符串数组，例如 Guids = [\"32位SDL_GUID\"]",
                ValueTypeNameWide(entry.value.type),
                L"即使只有一个 GUID，也请使用字符串数组。");
            return false;
        }
        if (entry.value.strings.empty() || entry.value.strings.size() > MaxControllerGuids) {
            SetEntryError(
                config, entry,
                L"GUID 数组数量无效。",
                L"1～16 个 GUID",
                L"空数组或数量超限",
                L"运行 JoyShimGamepadInfo.exe，复制输出的 GUID。");
            return false;
        }
        controller.guids.clear();
        for (const std::string& guid : entry.value.strings) {
            if (!IsHexGuid(guid)) {
                SetEntryError(
                    config, entry,
                    L"GUID 格式无效。",
                    L"恰好 32 位十六进制字符串",
                    L"长度或字符不正确",
                    L"不要手工猜测；请复制 JoyShimGamepadInfo.exe 输出的 SDL GUID。");
                return false;
            }
            std::array<char, 33> normalized{};
            NormalizeGuid(guid, normalized.data());
            for (const auto& existing : controller.guids) {
                if (EqualsAsciiIgnoreCase(existing.data(), normalized.data())) {
                    SetEntryError(
                        config, entry,
                        L"Guids 包含重复项。",
                        L"每个 GUID 只出现一次",
                        L"重复 GUID",
                        L"删除重复项；数组内部按 OR 匹配。");
                    return false;
                }
            }
            controller.guids.push_back(normalized);
        }
    } else if (entry.key == "Names") {
        return ParseDeviceTextArray(
            controller.names, config, entry,
            L"填写一个或多个完整 SDL 设备名称。");
    } else if (entry.key == "NameContains") {
        return ParseDeviceTextArray(
            controller.nameContains, config, entry,
            L"填写一个或多个稳定名称片段。");
    } else if (entry.key == "Serials") {
        return ParseDeviceTextArray(
            controller.serials, config, entry,
            L"填写一个或多个完整序列号。");
    } else if (entry.key == "Paths") {
        return ParseDevicePathArray(controller.paths, config, entry);
    } else if (entry.key == "DeviceIds") {
        return ParseDeviceIds(controller, config, entry);
    } else if (entry.key == "Connections") {
        return ParseConnections(controller, config, entry);
    } else if (entry.key == "Name" || entry.key == "Serial" ||
               entry.key == "Vendor" || entry.key == "Product" ||
               entry.key == "Connection") {
        SetEntryError(
            config, entry,
            L"旧的单值设备匹配字段已移除。",
            L"Names、NameContains、Serials、Paths、DeviceIds 或 Connections 数组",
            L"旧字段",
            L"多个候选值应写在同一个数组中；VID/PID 使用 DeviceIds = [\"VVVV:PPPP\"]。");
        return false;
    } else {
        return SetUnknownFieldError(
            config, entry,
            L"Enabled、Priority、MatchAny、Serials、Paths、DeviceIds、Names、NameContains、Guids、Connections");
    }
    return true;
}

bool ParseProjectRoot(
    ProjectConfig& project,
    RuntimeConfig& config,
    const toml::Entry& entry) noexcept {
    if (entry.key == "ProcessNames") {
        return ParseProcessNames(project, config, entry);
    }
    if (entry.key == "Enabled" || entry.key == "Priority" ||
        entry.key == "Process" || entry.key == "ProcessContains") {
        SetEntryError(
            config,
            entry,
            L"旧的多游戏配置字段已移除。",
            L"Project.ProcessNames 字符串数组",
            L"旧字段",
            L"JoyShim 只服务当前游戏；精确同名配置无需填写 Project。只有同一游戏包含多个 EXE 时才使用 [Project].ProcessNames。");
        return false;
    }
    return SetUnknownFieldError(config, entry, L"ProcessNames");
}

bool ParseCorrectionEntry(
    AxisCorrectionPatch& patch,
    RuntimeConfig& config,
    const toml::Entry& entry) noexcept {
    bool value = false;
    if (entry.key != "SwapXY" && entry.key != "InvertX" && entry.key != "InvertY") {
        return SetUnknownFieldError(config, entry, L"SwapXY、InvertX、InvertY");
    }
    if (!ExpectBoolean(entry, value, config)) return false;
    if (entry.key == "SwapXY") {
        patch.swapConfigured = true;
        patch.value.swapXY = value;
    } else if (entry.key == "InvertX") {
        patch.invertXConfigured = true;
        patch.value.invertX = value;
    } else {
        patch.invertYConfigured = true;
        patch.value.invertY = value;
    }
    return true;
}

bool ParseOutputEntry(
    AxisOutputPatch& patch,
    RuntimeConfig& config,
    const toml::Entry& entry) noexcept {
    if (entry.key == "SwapXY" || entry.key == "InvertX" || entry.key == "InvertY") {
        return ParseCorrectionEntry(patch.correction, config, entry);
    }
    std::int64_t integer = 0;
    bool boolean = false;
    const std::string* string = nullptr;
    if (entry.key == "MatrixXX" || entry.key == "MatrixXY" ||
        entry.key == "MatrixYX" || entry.key == "MatrixYY") {
        if (!ExpectInteger(entry, -400, 400, integer, config)) return false;
        if (entry.key == "MatrixXX") {
            patch.matrixXXConfigured = true;
            patch.value.matrix.xx = static_cast<std::int32_t>(integer);
        } else if (entry.key == "MatrixXY") {
            patch.matrixXYConfigured = true;
            patch.value.matrix.xy = static_cast<std::int32_t>(integer);
        } else if (entry.key == "MatrixYX") {
            patch.matrixYXConfigured = true;
            patch.value.matrix.yx = static_cast<std::int32_t>(integer);
        } else {
            patch.matrixYYConfigured = true;
            patch.value.matrix.yy = static_cast<std::int32_t>(integer);
        }
    } else if (entry.key == "AvoidExactDiagonals") {
        if (!ExpectBoolean(entry, boolean, config)) return false;
        patch.diagonalEnabledConfigured = true;
        patch.value.diagonal.enabled = boolean;
    } else if (entry.key == "DiagonalTolerance" || entry.key == "DiagonalOffset" ||
        entry.key == "DiagonalMinimumMagnitude" || entry.key == "DiagonalMinimumGap") {
        if (!ExpectInteger(entry, 0, 32767, integer, config)) return false;
        if (entry.key == "DiagonalTolerance") {
            patch.diagonalToleranceConfigured = true;
            patch.value.diagonal.tolerance = static_cast<std::int32_t>(integer);
        } else if (entry.key == "DiagonalOffset") {
            if (integer > 0 && patch.diagonalMinimumGapConfigured &&
                patch.value.diagonal.minimumGap > 0) {
                SetEntryError(
                    config, entry,
                    L"DiagonalOffset 与 DiagonalMinimumGap 不能同时启用。",
                    L"二选一：旧的固定偏移，或新的最小主轴差值",
                    L"两个模式同时为非零",
                    L"Blood Stone 测试使用 DiagonalOffset = 0 和 DiagonalMinimumGap = 4096。");
                return false;
            }
            patch.diagonalOffsetConfigured = true;
            patch.value.diagonal.offset = static_cast<std::int32_t>(integer);
        } else if (entry.key == "DiagonalMinimumMagnitude") {
            patch.diagonalMinimumMagnitudeConfigured = true;
            patch.value.diagonal.minimumMagnitude = static_cast<std::int32_t>(integer);
        } else {
            if (integer > 0 && patch.diagonalOffsetConfigured &&
                patch.value.diagonal.offset > 0) {
                SetEntryError(
                    config, entry,
                    L"DiagonalMinimumGap 与 DiagonalOffset 不能同时启用。",
                    L"二选一：新的最小主轴差值，或旧的固定偏移",
                    L"两个模式同时为非零",
                    L"Blood Stone 测试使用 DiagonalMinimumGap = 4096 和 DiagonalOffset = 0。");
                return false;
            }
            patch.diagonalMinimumGapConfigured = true;
            patch.value.diagonal.minimumGap = static_cast<std::int32_t>(integer);
        }
    } else if (entry.key == "DiagonalBiasAxis") {
        if (!ExpectString(entry, string, config)) return false;
        patch.diagonalAxisConfigured = true;
        if (*string == "X") patch.value.diagonal.axis = DiagonalBiasAxis::X;
        else if (*string == "Y") patch.value.diagonal.axis = DiagonalBiasAxis::Y;
        else {
            SetEntryError(
                config, entry,
                L"DiagonalBiasAxis 枚举值无效。",
                L"\"X\" 或 \"Y\"",
                L"未知字符串",
                L"Blood Stone 默认使用 DiagonalBiasAxis = \"X\"。");
            return false;
        }
    } else if (entry.key == "DiagonalSignMode") {
        if (!ExpectString(entry, string, config)) return false;
        patch.diagonalSignModeConfigured = true;
        if (*string == "All") patch.value.diagonal.signMode = DiagonalSignMode::All;
        else if (*string == "SameSigns") patch.value.diagonal.signMode = DiagonalSignMode::SameSigns;
        else if (*string == "OppositeSigns") patch.value.diagonal.signMode = DiagonalSignMode::OppositeSigns;
        else {
            SetEntryError(
                config, entry,
                L"DiagonalSignMode 枚举值无效。",
                L"\"All\"、\"SameSigns\" 或 \"OppositeSigns\"",
                L"未知字符串",
                L"左上/右下这组异号方向应使用 \"OppositeSigns\"。");
            return false;
        }
    } else if (entry.key == "AxisSeparationMode") {
        if (!ExpectString(entry, string, config)) return false;
        patch.separationModeConfigured = true;
        if (*string == "None") {
            patch.value.separation.mode = AxisSeparationMode::None;
        } else if (*string == "PreferX") {
            patch.value.separation.mode = AxisSeparationMode::PreferX;
        } else if (*string == "PreferY") {
            patch.value.separation.mode = AxisSeparationMode::PreferY;
        } else if (*string == "DominantAxis") {
            patch.value.separation.mode = AxisSeparationMode::DominantAxis;
        } else {
            SetEntryError(
                config, entry,
                L"AxisSeparationMode 枚举值无效。",
                L"\"None\"、\"PreferX\"、\"PreferY\" 或 \"DominantAxis\"",
                L"未知字符串",
                L"Blood Stone 第一轮象限测试使用 AxisSeparationMode = \"PreferX\"。");
            return false;
        }
    } else if (entry.key == "AxisSeparationMinimumMagnitude") {
        if (!ExpectInteger(entry, 0, 32767, integer, config)) return false;
        patch.separationMinimumMagnitudeConfigured = true;
        patch.value.separation.minimumMagnitude = static_cast<std::int32_t>(integer);
    } else if (entry.key == "AxisSeparationSecondaryPercent") {
        if (!ExpectInteger(entry, 0, 100, integer, config)) return false;
        patch.separationSecondaryPercentConfigured = true;
        patch.value.separation.secondaryPercent = static_cast<std::int32_t>(integer);
    } else if (entry.key == "AxisSeparationSignMode") {
        if (!ExpectString(entry, string, config)) return false;
        patch.separationSignModeConfigured = true;
        if (*string == "All") patch.value.separation.signMode = DiagonalSignMode::All;
        else if (*string == "SameSigns") patch.value.separation.signMode = DiagonalSignMode::SameSigns;
        else if (*string == "OppositeSigns") patch.value.separation.signMode = DiagonalSignMode::OppositeSigns;
        else {
            SetEntryError(
                config, entry,
                L"AxisSeparationSignMode 枚举值无效。",
                L"\"All\"、\"SameSigns\" 或 \"OppositeSigns\"",
                L"未知字符串",
                L"左上/右下测试使用 AxisSeparationSignMode = \"OppositeSigns\"。");
            return false;
        }
    } else {
        return SetUnknownFieldError(
            config, entry,
            L"SwapXY、InvertX、InvertY、MatrixXX、MatrixXY、MatrixYX、MatrixYY、AvoidExactDiagonals、DiagonalTolerance、DiagonalOffset、DiagonalMinimumMagnitude、DiagonalMinimumGap、DiagonalBiasAxis、DiagonalSignMode、AxisSeparationMode、AxisSeparationMinimumMagnitude、AxisSeparationSecondaryPercent、AxisSeparationSignMode");
    }
    return true;
}

int ButtonTargetForKey(const std::string& key) noexcept {
    static constexpr const char* names[] = {
        "DpadUp", "DpadDown", "DpadLeft", "DpadRight", "Start", "Back",
        "LeftThumb", "RightThumb", "LeftShoulder", "RightShoulder", "Guide",
        "A", "B", "X", "Y",
    };
    for (std::size_t index = 0; index < std::size(names); ++index) {
        if (key == names[index]) return static_cast<int>(index);
    }
    return -1;
}

int SignedAxisForKey(const std::string& key) noexcept {
    if (key == "LeftStickX") return 0;
    if (key == "LeftStickY") return 1;
    if (key == "RightStickX") return 2;
    if (key == "RightStickY") return 3;
    return -1;
}

int HalfAxisForKey(const std::string& key) noexcept {
    static constexpr const char* names[] = {
        "LeftStickLeft", "LeftStickRight", "LeftStickUp", "LeftStickDown",
        "RightStickLeft", "RightStickRight", "RightStickUp", "RightStickDown",
        "LeftTrigger", "RightTrigger",
    };
    for (std::size_t index = 0; index < std::size(names); ++index) {
        if (key == names[index]) return static_cast<int>(index);
    }
    return -1;
}

bool ParseRoutingEntry(
    RoutingPatch& patch,
    bool buttons,
    RuntimeConfig& config,
    const toml::Entry& entry) noexcept {
    const std::string* text = nullptr;
    if (!ExpectString(entry, text, config)) return false;
    wchar_t wide[512]{};
    if (!Utf8ToWide(*text, wide, static_cast<int>(std::size(wide)))) {
        SetEntryError(
            config, entry,
            L"按键或轴来源表达式无效。",
            L"有效 UTF-8 字符串",
            L"编码无效或过长",
            L"从默认 TOML 的 Buttons/Axes 示例中复制表达式再修改。");
        return false;
    }
    if (buttons) {
        const int target = ButtonTargetForKey(entry.key);
        if (target < 0) {
            return SetUnknownFieldError(
                config, entry,
                L"DpadUp、DpadDown、DpadLeft、DpadRight、Start、Back、LeftThumb、RightThumb、LeftShoulder、RightShoulder、Guide、A、B、X、Y");
        }
        ButtonBinding parsed{};
        if (!ParseButtonBinding(wide, parsed)) {
            SetEntryError(
                config, entry,
                L"无效按键来源表达式。",
                L"Gamepad.<标准按键>，可用 | 连接多个来源",
                L"无法解析的字符串",
                L"示例：Guide = \"Gamepad.Guide|Gamepad.Misc1\"。");
            return false;
        }
        patch.buttonConfigured[static_cast<std::size_t>(target)] = true;
        patch.buttons[static_cast<std::size_t>(target)] = parsed;
        return true;
    }
    const int signedIndex = SignedAxisForKey(entry.key);
    if (signedIndex >= 0) {
        SignedAxisSource parsed{};
        if (!ParseSignedAxisSource(wide, parsed)) {
            SetEntryError(
                config, entry,
                L"无效完整轴来源表达式。",
                L"Gamepad.<轴> 或 Gamepad.<轴>.Inverted",
                L"无法解析的字符串",
                L"示例：RightStickY = \"Gamepad.RightY.Inverted\"。");
            return false;
        }
        patch.signedConfigured[static_cast<std::size_t>(signedIndex)] = true;
        patch.signedAxes[static_cast<std::size_t>(signedIndex)] = parsed;
        return true;
    }
    const int halfIndex = HalfAxisForKey(entry.key);
    if (halfIndex >= 0) {
        HalfAxisSource parsed{};
        if (!ParseHalfAxisSource(wide, parsed)) {
            SetEntryError(
                config, entry,
                L"无效半轴来源表达式。",
                L"Gamepad.<轴>.Positive、Negative 或 None",
                L"无法解析的字符串",
                L"半轴只用于兼容特殊映射；普通手柄优先使用完整轴字段。");
            return false;
        }
        patch.halfConfigured[static_cast<std::size_t>(halfIndex)] = true;
        patch.halfAxes[static_cast<std::size_t>(halfIndex)] = parsed;
        return true;
    }
    return SetUnknownFieldError(
        config, entry,
        L"LeftStickX、LeftStickY、RightStickX、RightStickY、LeftStickLeft/Right/Up/Down、RightStickLeft/Right/Up/Down、LeftTrigger、RightTrigger");
}

bool ParseNativeInputEntry(
    NativeInputPatch& patch,
    RuntimeConfig& config,
    const toml::Entry& entry) noexcept {
    bool value = false;
    if (!ExpectBoolean(entry, value, config)) return false;
    // HideDirectInput 是对外统一名称；旧 BlockDirectInput 继续兼容。
    // 两个别名不得在同一个作用域内同时填写，以免配置结果受书写顺序影响。
    if (entry.key == "HideDirectInput" || entry.key == "BlockDirectInput") {
        if (patch.blockDirectInputConfigured) {
            SetEntryError(
                config, entry,
                L"HideDirectInput 与 BlockDirectInput 是同一功能，不得同时填写。",
                L"每个 NativeInput 表中只允许一个隐藏 DirectInput 选项",
                L"重复声明隐藏策略",
                L"新配置使用 HideDirectInput；迁移旧配置时删除 BlockDirectInput。");
            return false;
        }
        patch.blockDirectInputConfigured = true;
        patch.blockDirectInput = value;
        return true;
    }
    if (entry.key == "TraceDirectInput") {
        patch.traceDirectInputConfigured = true;
        patch.traceDirectInput = value;
        return true;
    }
    return SetUnknownFieldError(
        config,
        entry,
        L"HideDirectInput、TraceDirectInput（兼容旧字段 BlockDirectInput）");
}

bool ParseProfileSubtable(
    ProfilePatch& patch,
    const std::vector<std::string>& suffix,
    RuntimeConfig& config,
    const toml::Entry& entry) noexcept {
    if (suffix.size() == 1 && suffix[0] == "Buttons") {
        return ParseRoutingEntry(patch.routing, true, config, entry);
    }
    if (suffix.size() == 1 && suffix[0] == "Axes") {
        return ParseRoutingEntry(patch.routing, false, config, entry);
    }
    if (suffix.size() == 1 && suffix[0] == "NativeInput") {
        return ParseNativeInputEntry(patch.nativeInput, config, entry);
    }
    if (suffix.size() == 2 && suffix[0] == "Input" && suffix[1] == "LeftStick") {
        return ParseCorrectionEntry(patch.inputLeft, config, entry);
    }
    if (suffix.size() == 2 && suffix[0] == "Input" && suffix[1] == "RightStick") {
        return ParseCorrectionEntry(patch.inputRight, config, entry);
    }
    if (suffix.size() == 2 && suffix[0] == "Output" && suffix[1] == "LeftStick") {
        return ParseOutputEntry(patch.outputLeft, config, entry);
    }
    if (suffix.size() == 2 && suffix[0] == "Output" && suffix[1] == "RightStick") {
        return ParseOutputEntry(patch.outputRight, config, entry);
    }
    return SetUnknownTableError(
        config, entry,
        L"Buttons、Axes、NativeInput、Input.LeftStick、Input.RightStick、Output.LeftStick、Output.RightStick");
}

bool IsProfileSuffix(const std::vector<std::string>& suffix) noexcept {
    if (suffix.size() == 1 &&
        (suffix[0] == "Buttons" || suffix[0] == "Axes" || suffix[0] == "NativeInput")) {
        return true;
    }
    return suffix.size() == 2 &&
        (suffix[0] == "Input" || suffix[0] == "Output") &&
        (suffix[1] == "LeftStick" || suffix[1] == "RightStick");
}

void SetTableError(
    RuntimeConfig& config,
    const toml::TableDecl& table,
    const wchar_t* message,
    const wchar_t* expected,
    const wchar_t* suggestion) noexcept {
    wchar_t path[384]{};
    wchar_t source[768]{};
    CopyUtf8Text(toml::JoinPath(table.path), path, std::size(path));
    CopyUtf8Text(table.sourceLine, source, std::size(source));
    SetConfigError(
        config,
        table.line,
        table.column,
        path,
        message,
        expected,
        L"不受支持的表路径",
        suggestion,
        source);
}

void SetValidationErrorAtTable(
    RuntimeConfig& config,
    const toml::Document& document,
    const std::vector<std::string>& pathParts,
    const wchar_t* message,
    const wchar_t* expected,
    const wchar_t* actual,
    const wchar_t* suggestion) noexcept {
    wchar_t path[384]{};
    CopyUtf8Text(toml::JoinPath(pathParts), path, std::size(path));
    const toml::TableDecl* table = toml::FindTableDecl(document, pathParts);
    if (table == nullptr) {
        SetConfigError(
            config, 0, 0, path, message, expected, actual, suggestion, L"");
        return;
    }
    wchar_t source[768]{};
    CopyUtf8Text(table->sourceLine, source, std::size(source));
    SetConfigError(
        config,
        table->line,
        table->column,
        path,
        message,
        expected,
        actual,
        suggestion,
        source);
}

bool ValidateAndCreateTables(
    const toml::Document& document,
    RuntimeConfig& config) noexcept {
    config.project = {};
    config.controllers.clear();
    config.controllers.reserve(MaxControllerProfiles);

    for (const toml::TableDecl& table : document.tables) {
        const auto& path = table.path;
        if (path.empty()) continue;
        // [Input]、[Compatibility]、[Debug] 是正式公开的简化配置。
        // 仅接受预先定义的字段，仍保留旧版严格校验而非忽略未知参数。
        if (path.size() == 1 &&
            (path[0] == "Input" || path[0] == "Compatibility" || path[0] == "Debug")) {
            continue;
        }
        if (path[0] == "Global") {
            if ((path.size() == 1) ||
                (path.size() == 2 &&
                 (path[1] == "HotReload" || path[1] == "MappingDatabase" || path[1] == "Diagnostics"))) {
                continue;
            }
            SetTableError(
                config, table,
                L"未知 Global 子表。",
                L"Global、Global.HotReload、Global.MappingDatabase、Global.Diagnostics",
                L"Global 只保存轮询、日志、热重载和映射数据库等根本运行参数；输入行为请使用主项目功能表。");
            return false;
        }
        if (path[0] == "Project") {
            if (path.size() == 1) continue;
            SetTableError(
                config, table,
                L"Project 不支持子表。",
                L"Project",
                L"[Project] 只在同一游戏含多个 EXE 时填写 ProcessNames；普通输入设置直接放在 [NativeInput]、[Axes] 等主项目功能表中。");
            return false;
        }
        if (IsProfileSuffix(path)) {
            continue;
        }
        if (path[0] == "Controllers") {
            if (path.size() == 1) continue;
            ControllerProfileConfig* controller = FindOrCreateController(config, path[1]);
            if (controller == nullptr) {
                SetTableError(
                    config, table,
                    L"手柄定义数量过多或自定义名称过长。",
                    L"最多 32 个手柄定义，自定义名称少于 96 字节",
                    L"缩短名称或删除不再使用的手柄定义。");
                return false;
            }
            if (path.size() == 2) continue;
            const std::vector<std::string> suffix(path.begin() + 2, path.end());
            if (IsProfileSuffix(suffix)) continue;
            SetTableError(
                config, table,
                L"Controllers 下出现未知子表。",
                L"Controllers.<自定义名称> 及其 Buttons、Axes、NativeInput、Input/Output.LeftStick/RightStick",
                L"自定义名称只负责标识手柄子集合；功能设置必须放在该名称下面的标准功能子表中。");
            return false;
        }
        if (path[0] == "Games") {
            SetTableError(
                config, table,
                L"旧的 Games 多游戏层级已移除。",
                L"Project、主项目功能表，或 Controllers.<自定义名称>",
                L"DLL/Mod 只服务当前游戏：将 Games.<游戏>.Common.<功能> 移到顶层 <功能>，将游戏手柄规则直接移到 Controllers.<自定义名称>.<功能>。");
            return false;
        }
        SetTableError(
            config, table,
            L"未知顶层命名空间。",
            L"Input、Compatibility、Debug、Global、Project、Buttons、Axes、NativeInput、Input/Output.LeftStick/RightStick 或 Controllers",
            L"主项目功能表作用于当前游戏的全部手柄；Controllers.<自定义名称> 功能子表只覆盖对应手柄子集合。");
        return false;
    }
    return true;
}

// 极简表与旧版高级表允许分别使用，但相同设置不得在两处重复声明。
// 这样可以防止两处相同选项出现冲突而使配置结果由填写顺序决定。
bool CheckShortcutAliases(
    const toml::Document& document,
    RuntimeConfig& config) noexcept {
    const toml::Entry* shortcutHide = toml::FindEntry(
        document, {"Compatibility"}, "HideDirectInput");
    if (shortcutHide != nullptr &&
        (toml::FindEntry(document, {"NativeInput"}, "HideDirectInput") != nullptr ||
         toml::FindEntry(document, {"NativeInput"}, "BlockDirectInput") != nullptr)) {
        SetEntryError(
            config, *shortcutHide,
            L"DirectInput 隐藏选项同时出现在简化表和旧表。",
            L"[Compatibility].HideDirectInput 或 [NativeInput].HideDirectInput，二选一",
            L"同一游戏父集合重复配置",
            L"删除重复字段；手柄子集合中的 NativeInput 不受影响。");
        return false;
    }
    const toml::Entry* shortcutLog = toml::FindEntry(document, {"Debug"}, "Log");
    if (shortcutLog != nullptr &&
        toml::FindEntry(document, {"Global", "Diagnostics"}, "Enabled") != nullptr) {
        SetEntryError(
            config, *shortcutLog,
            L"日志开关同时出现在简化表和旧表。",
            L"[Debug].Log 或 [Global.Diagnostics].Enabled，二选一",
            L"同一日志开关重复配置",
            L"删除其中一个 Log/Enabled 字段，以免热重载时结果不明确。");
        return false;
    }
    return true;
}

bool BuildConfiguration(
    HMODULE module,
    const toml::Document& document,
    RuntimeConfig& config) noexcept {
    if (!ValidateAndCreateTables(document, config)) return false;
    if (!CheckShortcutAliases(document, config)) return false;

    for (const toml::Entry& entry : document.entries) {
        const auto& path = entry.table;
        if (path.empty()) {
            return SetUnknownTableError(
                config, entry,
                L"所有配置项必须位于 [Global]、[Project]、主项目功能表或 [Controllers.<自定义名称>] 中");
        }
        if (path.size() == 1 && path[0] == "Input") {
            if (entry.key != "Rumble") {
                return SetUnknownFieldError(config, entry, L"Input 仅接受 Rumble");
            }
            if (!ExpectBoolean(entry, config.enableRumble, config)) return false;
            continue;
        }
        if (path.size() == 1 && path[0] == "Compatibility") {
            if (entry.key != "HideDirectInput") {
                return SetUnknownFieldError(config, entry, L"Compatibility 仅接受 HideDirectInput");
            }
            // 简化表写入项目级 NativeInput 策略，所有手柄默认继承，
            // Controllers.<名称>.NativeInput 仍能针对具体设备覆盖。
            if (!ParseNativeInputEntry(config.project.patch.nativeInput, config, entry)) return false;
            continue;
        }
        if (path.size() == 1 && path[0] == "Debug") {
            if (entry.key != "Log") {
                return SetUnknownFieldError(config, entry, L"Debug 仅接受 Log");
            }
            if (!ExpectBoolean(entry, config.enableLog, config)) return false;
            continue;
        }
        if (path[0] == "Global") {
            if (!ParseGlobalEntry(module, config, entry)) return false;
            continue;
        }
        if (path[0] == "Project") {
            if (path.size() != 1) {
                return SetUnknownTableError(config, entry, L"Project");
            }
            if (!ParseProjectRoot(config.project, config, entry)) return false;
            continue;
        }
        if (IsProfileSuffix(path)) {
            if (!ParseProfileSubtable(config.project.patch, path, config, entry)) return false;
            continue;
        }
        if (path[0] == "Controllers") {
            if (path.size() < 2) {
                return SetUnknownTableError(config, entry, L"Controllers.<自定义名称>");
            }
            ControllerProfileConfig* controller = FindOrCreateController(config, path[1]);
            if (controller == nullptr) return false;
            if (path.size() == 2) {
                if (!ParseControllerRoot(*controller, config, entry)) return false;
            } else {
                const std::vector<std::string> suffix(path.begin() + 2, path.end());
                if (!ParseProfileSubtable(controller->patch, suffix, config, entry)) return false;
            }
            continue;
        }
        if (path[0] == "Games") {
            return SetUnknownTableError(
                config, entry,
                L"旧 Games 层级已移除；使用顶层功能表和 Controllers.<自定义名称>.<功能>");
        }
        return SetUnknownTableError(
            config, entry,
            L"Input、Compatibility、Debug、Global、Project、主项目功能表或 Controllers.<自定义名称>");
    }

    for (const ControllerProfileConfig& controller : config.controllers) {
        // Connections 只是身份命中后的附加限定，单独填写不会产生任何
        // 身份命中。它不能被计入 hasIdentity，否则配置会通过校验却永远
        // 无法匹配。需要“任意有线/无线手柄”时应显式配合 MatchAny。
        const bool hasIdentity = !controller.serials.empty() || !controller.paths.empty() ||
            !controller.deviceIds.empty() || !controller.names.empty() ||
            !controller.nameContains.empty() || !controller.guids.empty();
        if (controller.enabled && !controller.matchAny && !hasIdentity) {
            SetValidationErrorAtTable(
                config,
                document,
                {"Controllers", controller.alias},
                L"已启用的手柄子集合没有任何识别条件。",
                L"Serials、Paths、DeviceIds、Names、NameContains、Guids，或显式 MatchAny = true",
                L"无识别条件",
                L"推荐优先填写 DeviceIds；Connections 只能作为附加限定，若只按连接方式分组请同时设置 MatchAny = true；Guids 永远最后兜底。");
            return false;
        }
    }
    return true;
}

bool ProcessNamesContain(
    const ProjectConfig& project,
    const wchar_t* processName) noexcept {
    for (const ProcessNameValue& configured : project.processNames) {
        if (EqualsWideIgnoreCase(configured.value, processName)) return true;
    }
    return false;
}

bool BetterCandidate(
    std::int32_t priority,
    std::uint32_t order,
    std::int32_t selectedPriority,
    std::uint32_t selectedOrder,
    bool hasSelected) noexcept {
    if (!hasSelected) return true;
    if (priority != selectedPriority) return priority > selectedPriority;
    return order < selectedOrder;
}

enum class DefaultFileResult {
    AlreadyExists,
    Generated,
    Failed,
};

DefaultFileResult EnsureDefaultToml(const wchar_t* path) noexcept {
    if (path == nullptr || path[0] == L'\0') return DefaultFileResult::Failed;
    const DWORD attributes = GetFileAttributesW(path);
    if (attributes != INVALID_FILE_ATTRIBUTES) return DefaultFileResult::AlreadyExists;
    HANDLE file = CreateFileW(
        path,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS
            ? DefaultFileResult::AlreadyExists : DefaultFileResult::Failed;
    }
    DWORD written = 0;
    const bool success = WriteFile(
        file,
        DefaultTomlTemplate,
        static_cast<DWORD>(DefaultTomlTemplateSize),
        &written,
        nullptr) && written == DefaultTomlTemplateSize;
    FlushFileBuffers(file);
    CloseHandle(file);
    if (!success) {
        DeleteFileW(path);
        return DefaultFileResult::Failed;
    }
    return DefaultFileResult::Generated;
}

bool ReadWholeFile(
    const wchar_t* path,
    std::vector<char>& output,
    RuntimeConfig& config) noexcept {
    HANDLE file = CreateFileW(
        path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        config.configReadFailed = true;
        SetConfigError(
            config, 0, 0, L"当前代理的 TOML",
            L"无法读取配置文件。", L"可读的 UTF-8 文件", L"打开失败",
            L"检查文件权限、占用状态和路径；DLL 不会覆盖现有文件。", L"");
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        size.QuadPart > static_cast<LONGLONG>(MaxTomlBytes)) {
        CloseHandle(file);
        config.configReadFailed = true;
        SetConfigError(
            config, 0, 0, L"当前代理的 TOML",
            L"配置文件过大或无法读取文件大小。", L"不超过 2 MiB", L"文件过大或文件系统错误",
            L"删除无关的大段数据；注释可以保留，但配置文件总大小不得超过 2 MiB。", L"");
        return false;
    }
    output.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const bool success = output.empty() || ReadFile(
        file, output.data(), static_cast<DWORD>(output.size()), &read, nullptr);
    CloseHandle(file);
    if (!success || read != output.size()) {
        output.clear();
        config.configReadFailed = true;
        SetConfigError(
            config, 0, 0, L"当前代理的 TOML",
            L"读取配置文件时发生错误。", L"完整读取", L"短读或 I/O 错误",
            L"关闭占用该文件的编辑器后重试，并检查磁盘/权限状态。", L"");
        return false;
    }
    return true;
}

DWORD WINAPI AsyncLogWorker(void* parameter) noexcept {
    AsyncLogItem* item = static_cast<AsyncLogItem*>(parameter);
    if (item == nullptr) return 0;
    const ULONGLONG notBefore = static_cast<ULONGLONG>(
        InterlockedCompareExchange64(&g_logNotBeforeTick, 0, 0));
    const ULONGLONG now = GetTickCount64();
    if (notBefore > now) {
        const ULONGLONG delay = notBefore - now;
        Sleep(delay > MAXDWORD ? MAXDWORD : static_cast<DWORD>(delay));
    }
    AcquireSRWLockExclusive(&g_logFileLock);
    HANDLE file = CreateFileW(
        item->path,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, item->utf8, item->bytes, &written, nullptr);
        CloseHandle(file);
    }
    ReleaseSRWLockExclusive(&g_logFileLock);
    HeapFree(GetProcessHeap(), 0, item);
    return 0;
}

bool WriteConfigErrorReport(const RuntimeConfig& config) noexcept {
    if (config.errorReportPath[0] == L'\0') return false;
    wchar_t report[8192]{};
    wchar_t caret[512]{};
    if (config.parseErrorColumn > 0 && config.parseErrorColumn < std::size(caret) - 2) {
        for (std::uint32_t index = 1; index < config.parseErrorColumn; ++index) {
            caret[index - 1] = L' ';
        }
        caret[config.parseErrorColumn - 1] = L'^';
        caret[config.parseErrorColumn] = L'\0';
    }
    const int count = std::swprintf(
        report,
        std::size(report),
        L"JoyShim v0.5.2-test12 严格 TOML 配置错误\r\n"
        L"============================================================\r\n"
        L"配置文件：%ls\r\n"
        L"位置：第 %u 行，第 %u 列\r\n"
        L"字段/表路径：%ls\r\n"
        L"错误：%ls\r\n"
        L"期望：%ls\r\n"
        L"实际：%ls\r\n"
        L"修复建议：%ls\r\n\r\n"
        L"原始行：\r\n%ls\r\n%ls\r\n\r\n"
        L"处理结果：原 TOML 未被修改。\r\n"
        L"启动阶段使用内部透明默认值；热重载阶段继续使用上一份有效配置。\r\n"
        L"修正 TOML 后保存；启用热重载时会自动重试，否则重启游戏。\r\n",
        config.tomlPath,
        static_cast<unsigned int>(config.parseErrorLine),
        static_cast<unsigned int>(config.parseErrorColumn),
        config.parseErrorPath[0] != L'\0' ? config.parseErrorPath : L"(未知)",
        config.parseError[0] != L'\0' ? config.parseError : L"未知错误",
        config.parseErrorExpected[0] != L'\0' ? config.parseErrorExpected : L"(未提供)",
        config.parseErrorActual[0] != L'\0' ? config.parseErrorActual : L"(未提供)",
        config.parseErrorSuggestion[0] != L'\0' ? config.parseErrorSuggestion : L"参考默认配置模板。",
        config.parseErrorSource[0] != L'\0' ? config.parseErrorSource : L"(无可用原始行)",
        caret[0] != L'\0' ? caret : L"");
    if (count <= 0) return false;
    char utf8[32768]{};
    const int bytes = WideCharToMultiByte(
        CP_UTF8, 0, report, count, utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr);
    if (bytes <= 0) return false;
    HANDLE file = CreateFileW(
        config.errorReportPath,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool success = WriteFile(file, utf8, static_cast<DWORD>(bytes), &written, nullptr) &&
        written == static_cast<DWORD>(bytes);
    FlushFileBuffers(file);
    CloseHandle(file);
    return success;
}

void ClearConfigErrorReport(const RuntimeConfig& config) noexcept {
    if (config.errorReportPath[0] != L'\0') DeleteFileW(config.errorReportPath);
}

bool FileExists(const wchar_t* path) noexcept {
    if (path == nullptr || path[0] == L'\0') return false;
    const DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool EndsWithWideIgnoreCase(const wchar_t* text, const wchar_t* suffix) noexcept {
    if (text == nullptr || suffix == nullptr) return false;
    const std::size_t textLength = std::wcslen(text);
    const std::size_t suffixLength = std::wcslen(suffix);
    if (suffixLength > textLength) return false;
    return EqualsWideIgnoreCase(text + textLength - suffixLength, suffix);
}

const wchar_t* WideBaseName(const wchar_t* path) noexcept {
    if (path == nullptr) return L"";
    const wchar_t* backslash = std::wcsrchr(path, L'\\');
    const wchar_t* slash = std::wcsrchr(path, L'/');
    const wchar_t* separator = backslash;
    if (slash != nullptr && (separator == nullptr || slash > separator)) separator = slash;
    return separator != nullptr ? separator + 1 : path;
}

bool BuildErrorReportPath(
    const wchar_t* tomlPath,
    wchar_t* output,
    std::size_t outputCount) noexcept {
    if (tomlPath == nullptr || output == nullptr || outputCount == 0) return false;
    constexpr wchar_t suffix[] = L".error.txt";
    const std::size_t pathLength = std::wcslen(tomlPath);
    if (pathLength + std::size(suffix) > outputCount) return false;
    std::wmemcpy(output, tomlPath, pathLength);
    std::wmemcpy(output + pathLength, suffix, std::size(suffix));
    return true;
}

bool BuildExactProfileName(
    const wchar_t* processName,
    const wchar_t* suffix,
    wchar_t* output,
    std::size_t outputCount) noexcept {
    if (!IsValidProcessFileName(processName) || output == nullptr || outputCount == 0) return false;
    const std::size_t processLength = std::wcslen(processName);
    if (suffix == nullptr || suffix[0] == L'\0') return false;
    const std::size_t stemLength = processLength - 4;
    const std::size_t suffixLength = std::wcslen(suffix);
    if (stemLength + suffixLength + 1 > outputCount) return false;
    std::wmemcpy(output, processName, stemLength);
    std::wmemcpy(output + stemLength, suffix, suffixLength + 1);
    return true;
}

bool ExtractProfileOwner(
    const wchar_t* path,
    const wchar_t* suffix,
    wchar_t* output,
    std::size_t outputCount) noexcept {
    if (output == nullptr || outputCount == 0) return false;
    output[0] = L'\0';
    const wchar_t* fileName = WideBaseName(path);
    if (suffix == nullptr || !EndsWithWideIgnoreCase(fileName, suffix)) return false;
    const std::size_t fileLength = std::wcslen(fileName);
    const std::size_t suffixLength = std::wcslen(suffix);
    const std::size_t stemLength = fileLength - suffixLength;
    if (stemLength == 0 || stemLength + 5 > outputCount) return false;
    std::wmemcpy(output, fileName, stemLength);
    std::wmemcpy(output + stemLength, L".exe", 5);
    return IsValidProcessFileName(output);
}

bool ValidateSpecializedOwnership(
    const RuntimeConfig& config,
    ConfigSourceKind source,
    const wchar_t* ownerProcess,
    const wchar_t* currentProcess) noexcept {
    const bool exactOwner = EqualsWideIgnoreCase(ownerProcess, currentProcess);
    if (config.project.processNames.empty()) {
        // 精确同名文件已经由文件名完成归属声明；只有从其他专版扫描匹配
        // 当前 EXE 时，才强制要求 [Project].ProcessNames。
        return source == ConfigSourceKind::ExactProcessProfile && exactOwner;
    }
    return ProcessNamesContain(config.project, ownerProcess) &&
        ProcessNamesContain(config.project, currentProcess);
}

void InitializeRuntimePaths(HMODULE module, RuntimeConfig& config) noexcept {
    config.configValid = true;
    wchar_t moduleStem[MAX_PATH]{};
    wchar_t genericName[MAX_PATH]{};
    wchar_t profileSuffix[MAX_PATH]{};
    if (BuildModuleConfigStem(module, moduleStem, std::size(moduleStem)) &&
        std::swprintf(genericName, std::size(genericName), L"%ls.toml", moduleStem) > 0 &&
        BuildProxyProfileSuffix(moduleStem, profileSuffix, std::size(profileSuffix))) {
        BuildSiblingPath(module, genericName, config.genericTomlPath, MAX_PATH);
        CopyProcessName(config.processName, MAX_PATH);
        wchar_t exactName[MAX_PATH]{};
        if (BuildExactProfileName(
                config.processName, profileSuffix, exactName, std::size(exactName))) {
            BuildSiblingPath(module, exactName, config.exactTomlPath, MAX_PATH);
        }
    }
    BuildSiblingPath(module, L"joyshim.log", config.logPath, MAX_PATH);
    BuildSiblingPath(module, L"gamecontrollerdb.txt", config.mappingDatabasePath, MAX_PATH);
}

RuntimeConfig ParseConfigFile(
    HMODULE module,
    const RuntimeConfig& paths,
    const wchar_t* selectedPath,
    ConfigSourceKind source,
    const wchar_t* ownerProcess,
    bool specialized,
    bool writeErrorReport) noexcept {
    RuntimeConfig config = paths;
    config.project = {};
    config.controllers.clear();
    config.configSource = source;
    config.specializedConfig = specialized;
    config.genericConfigIgnored = specialized && FileExists(config.genericTomlPath);
    CopyWideText(selectedPath, config.tomlPath, std::size(config.tomlPath));
    CopyWideText(ownerProcess, config.selectedProfileOwner, std::size(config.selectedProfileOwner));
    BuildErrorReportPath(config.tomlPath, config.errorReportPath, std::size(config.errorReportPath));

    std::vector<char> bytes;
    if (!ReadWholeFile(config.tomlPath, bytes, config)) {
        if (writeErrorReport) WriteConfigErrorReport(config);
        return config;
    }
    toml::Document document;
    toml::ParseError parseError;
    if (!toml::ParseDocument(bytes.data(), bytes.size(), document, parseError)) {
        SetParseError(config, parseError);
        if (writeErrorReport) WriteConfigErrorReport(config);
        return config;
    }
    if (!BuildConfiguration(module, document, config)) {
        if (writeErrorReport) WriteConfigErrorReport(config);
        return config;
    }
    if (specialized && !ValidateSpecializedOwnership(
            config, source, ownerProcess, config.processName)) {
        SetConfigError(
            config,
            0,
            0,
            L"Project.ProcessNames",
            L"游戏专版配置的文件归属无效。",
            L"精确同名专版可省略 [Project]；同游戏其他 EXE 复用此专版时，ProcessNames 必须同时包含文件名对应 EXE 与当前 EXE",
            L"非同名进程未声明，或 ProcessNames 未包含完整归属集合",
            L"例如 Game.winmm.toml 由 GameDX11.exe 复用时，填写 [Project] ProcessNames = [\"Game.exe\", \"GameDX11.exe\"]。",
            L"");
        if (writeErrorReport) WriteConfigErrorReport(config);
        return config;
    }
    config.configValid = true;
    config.configReadFailed = false;
    if (writeErrorReport) ClearConfigErrorReport(config);
    return config;
}

bool BuildSiblingWildcard(
    HMODULE module,
    const wchar_t* wildcard,
    wchar_t* output,
    std::size_t outputCount) noexcept {
    return BuildSiblingPath(module, wildcard, output, outputCount);
}

bool JoinSiblingCandidate(
    const wchar_t* wildcardPath,
    const wchar_t* fileName,
    wchar_t* output,
    std::size_t outputCount) noexcept {
    if (wildcardPath == nullptr || fileName == nullptr || output == nullptr || outputCount == 0) {
        return false;
    }
    CopyWideText(wildcardPath, output, outputCount);
    wchar_t* separator = std::wcsrchr(output, L'\\');
    if (separator == nullptr) separator = std::wcsrchr(output, L'/');
    if (separator == nullptr) return false;
    ++separator;
    const std::size_t prefixLength = static_cast<std::size_t>(separator - output);
    const std::size_t nameLength = std::wcslen(fileName);
    if (prefixLength + nameLength + 1 > outputCount) return false;
    std::wmemcpy(separator, fileName, nameLength + 1);
    return true;
}

RuntimeConfig MakeSelectionError(
    const RuntimeConfig& paths,
    const wchar_t* message,
    const wchar_t* actual,
    const wchar_t* suggestion) noexcept {
    RuntimeConfig config = paths;
    CopyWideText(paths.genericTomlPath, config.tomlPath, std::size(config.tomlPath));
    BuildErrorReportPath(config.tomlPath, config.errorReportPath, std::size(config.errorReportPath));
    SetConfigError(
        config, 0, 0, L"专版配置选择", message,
        L"当前进程最多匹配一份当前代理对应的 EXE 专用 TOML",
        actual,
        suggestion,
        L"");
    WriteConfigErrorReport(config);
    return config;
}

RuntimeConfig SelectAndLoadConfiguration(HMODULE module) noexcept {
    RuntimeConfig paths{};
    InitializeRuntimePaths(module, paths);
    wchar_t moduleStem[MAX_PATH]{};
    wchar_t profileSuffix[MAX_PATH]{};
    if (!BuildModuleConfigStem(module, moduleStem, std::size(moduleStem)) ||
        !BuildProxyProfileSuffix(moduleStem, profileSuffix, std::size(profileSuffix))) {
        return MakeSelectionError(paths, L"无法确定代理模块的配置文件名。",
                                  L"模块路径无效", L"确认代理 DLL 从本地有效路径加载。");
    }

    // 精确文件名是最强归属声明。存在时无论其内容是否有效，都不得回退。
    if (FileExists(paths.exactTomlPath)) {
        wchar_t owner[MAX_PATH]{};
        if (!ExtractProfileOwner(paths.exactTomlPath, profileSuffix, owner, std::size(owner))) {
            return MakeSelectionError(
                paths,
                L"无法从精确专版文件名解析主 EXE。",
                paths.exactTomlPath,
                L"使用 <EXE主名>.<代理DLL主名>.toml，例如 Game.winmm.toml。");
        }
        return ParseConfigFile(
            module,
            paths,
            paths.exactTomlPath,
            ConfigSourceKind::ExactProcessProfile,
            owner,
            true,
            true);
    }

    // 支持同一游戏多个 EXE：当前 EXE 没有同名专版时，扫描其他专版的
    // [Project].ProcessNames。文件名对应 EXE 与当前 EXE 必须位于同一项目归属集合。
    wchar_t wildcardPath[MAX_PATH]{};
    wchar_t wildcardName[MAX_PATH]{};
    std::swprintf(wildcardName, std::size(wildcardName), L"*%ls", profileSuffix);
    BuildSiblingWildcard(module, wildcardName, wildcardPath, std::size(wildcardPath));
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(wildcardPath, &data);
    RuntimeConfig selected{};
    unsigned int matches = 0;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
            if (EqualsWideIgnoreCase(data.cFileName, WideBaseName(paths.genericTomlPath))) continue;
            wchar_t candidatePath[MAX_PATH]{};
            wchar_t owner[MAX_PATH]{};
            if (!JoinSiblingCandidate(
                    wildcardPath, data.cFileName, candidatePath, std::size(candidatePath)) ||
                !ExtractProfileOwner(candidatePath, profileSuffix, owner, std::size(owner))) {
                continue;
            }
            RuntimeConfig candidate = ParseConfigFile(
                module,
                paths,
                candidatePath,
                ConfigSourceKind::SecondaryProcessProfile,
                owner,
                true,
                false);
            if (!candidate.configValid || candidate.configReadFailed) continue;
            ++matches;
            if (matches == 1) {
                selected = candidate;
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    if (matches == 1) {
        selected.genericConfigIgnored = FileExists(paths.genericTomlPath);
        ClearConfigErrorReport(selected);
        return selected;
    }
    if (matches > 1) {
        wchar_t actual[128]{};
        std::swprintf(actual, std::size(actual), L"匹配到 %u 份专版配置", matches);
        return MakeSelectionError(
            paths,
            L"当前进程的专版配置归属存在歧义。",
            actual,
            L"确保当前 EXE 只出现在一个专版的 ProcessNames 中；不要依赖文件顺序或 Priority 消除归属冲突。");
    }

    // 没有专版时才读取/生成当前目录默认配置。
    CopyWideText(paths.genericTomlPath, paths.tomlPath, std::size(paths.tomlPath));
    BuildErrorReportPath(paths.tomlPath, paths.errorReportPath, std::size(paths.errorReportPath));
    const DefaultFileResult defaultResult = EnsureDefaultToml(paths.genericTomlPath);
    paths.configGenerated = defaultResult == DefaultFileResult::Generated;
    paths.configSource = paths.configGenerated
        ? ConfigSourceKind::GeneratedGeneric
        : ConfigSourceKind::Generic;
    if (defaultResult == DefaultFileResult::Failed) {
        paths.configReadFailed = true;
        SetConfigError(
            paths, 0, 0, WideBaseName(paths.genericTomlPath),
            L"代理同名默认配置不存在且无法写出默认模板。",
            L"DLL/EXE 所在目录可写",
            L"创建文件失败",
            L"检查游戏目录权限；当前使用内部透明默认值继续运行。",
            L"");
        WriteConfigErrorReport(paths);
        return paths;
    }
    RuntimeConfig loaded = ParseConfigFile(
        module,
        paths,
        paths.genericTomlPath,
        paths.configSource,
        L"",
        false,
        true);
    loaded.configGenerated = paths.configGenerated;
    return loaded;
}

}  // namespace

RuntimeConfig LoadRuntimeConfig(HMODULE module) noexcept {
#ifdef BSCN_SINGLE_DLL
    RuntimeConfig config{};
    InitializeRuntimePaths(module, config);
    // 专用发行版没有配置文件，也不应自动创建配置文件或错误报告。
    config.enableLog = true;
    config.verboseDeviceInfo = false;
    config.enableHotReload = false;
    config.reloadMappingDatabase = false;
    config.enableExternalDatabase = false;
    config.logStartupDelayMs = 0;
    config.configValid = true;
    config.specializedConfig = true;
    // 原生 DirectInput 去重只允许指定 Stadia 设备；其他手柄保持原状。
    return config;
#else
    return SelectAndLoadConfiguration(module);
#endif
}

RuntimeConfig LoadRuntimeConfigFile(HMODULE module, const wchar_t* path) noexcept {
    RuntimeConfig paths{};
    InitializeRuntimePaths(module, paths);
    if (path == nullptr || path[0] == L'\0') {
        return MakeSelectionError(
            paths,
            L"未指定要验证的 TOML 文件。",
            L"空路径",
            L"传入 <代理DLL主名>.toml 或 <EXE主名>.<代理DLL主名>.toml。");
    }

    wchar_t selectedPath[MAX_PATH]{};
    if (std::wcschr(path, L'\\') != nullptr || std::wcschr(path, L'/') != nullptr ||
        (std::wcslen(path) >= 2 && path[1] == L':')) {
        CopyWideText(path, selectedPath, std::size(selectedPath));
    } else {
        BuildSiblingPath(module, path, selectedPath, std::size(selectedPath));
    }

    // 显式校验器不一定以同名代理 DLL 的形式运行。允许检查所有七种
    // 代理的配置名，但默认自动加载只认当前实际 DLL 的主名。
    const wchar_t* selectedName = WideBaseName(selectedPath);
    constexpr const wchar_t* proxyStems[] = {
        L"xinput1_1", L"xinput1_2", L"xinput1_3", L"xinput1_4",
        L"xinput9_1_0", L"dinput8", L"winmm",
    };
    wchar_t selectedSuffix[MAX_PATH]{};
    bool isGeneric = EqualsWideIgnoreCase(selectedName, WideBaseName(paths.genericTomlPath));
    for (const wchar_t* stem : proxyStems) {
        wchar_t genericName[64]{};
        wchar_t suffix[64]{};
        std::swprintf(genericName, std::size(genericName), L"%ls.toml", stem);
        BuildProxyProfileSuffix(stem, suffix, std::size(suffix));
        if (EqualsWideIgnoreCase(selectedName, genericName)) isGeneric = true;
        if (EndsWithWideIgnoreCase(selectedName, suffix)) {
            CopyWideText(suffix, selectedSuffix, std::size(selectedSuffix));
        }
    }
    if (isGeneric) {
        return ParseConfigFile(
            module, paths, selectedPath, ConfigSourceKind::Generic, L"", false, true);
    }

    wchar_t owner[MAX_PATH]{};
    if (!ExtractProfileOwner(selectedPath, selectedSuffix, owner, std::size(owner))) {
        return MakeSelectionError(
            paths,
            L"配置文件名不符合 JoyShim 专版命名规则。",
            WideBaseName(selectedPath),
            L"使用 winmm.toml，或 <EXE主名>.winmm.toml；其他代理使用自己的主名。");
    }
    CopyWideText(owner, paths.processName, std::size(paths.processName));
    return ParseConfigFile(
        module,
        paths,
        selectedPath,
        ConfigSourceKind::ExactProcessProfile,
        owner,
        true,
        true);
}

RuntimeConfig ReloadRuntimeConfig(
    HMODULE module,
    const RuntimeConfig& current) noexcept {
#ifdef BSCN_SINGLE_DLL
    (void)current;
    return LoadRuntimeConfig(module);
#else
    RuntimeConfig paths{};
    InitializeRuntimePaths(module, paths);
    CopyWideText(current.processName, paths.processName, std::size(paths.processName));
    CopyWideText(current.genericTomlPath, paths.genericTomlPath, std::size(paths.genericTomlPath));
    CopyWideText(current.exactTomlPath, paths.exactTomlPath, std::size(paths.exactTomlPath));

    const bool specialized = current.specializedConfig;
    return ParseConfigFile(
        module,
        paths,
        current.tomlPath,
        current.configSource,
        specialized ? current.selectedProfileOwner : L"",
        specialized,
        true);
#endif
}

DeviceProfile LoadDeviceProfile(
    const RuntimeConfig& config,
    const DeviceIdentity& identity) noexcept {
    DeviceProfile profile = MakeDefaultDeviceProfile();

    // 当前配置文件只服务当前游戏，因此主项目功能表就是所有已连接手柄
    // 的父集合默认值。它始终先应用，手柄子集合只能覆盖明确写出的项。
    ApplyProfilePatch(profile, config.project.patch);
    profile.projectPatchApplied = HasProfilePatch(config.project.patch);

    const ControllerProfileConfig* selectedController = nullptr;
    ControllerMatchKind selectedControllerMatch = ControllerMatchKind::None;
    std::int32_t selectedPriority = 0;
    std::uint32_t selectedOrder = 0;
    bool hasSelected = false;
    for (const ControllerProfileConfig& controller : config.controllers) {
        const ControllerMatchKind match = EvaluateControllerMatch(controller, identity);
        if (match == ControllerMatchKind::None) continue;
        const bool betterLayer = !hasSelected ||
            ControllerMatchRank(match) < ControllerMatchRank(selectedControllerMatch);
        const bool sameLayerBetter = hasSelected && match == selectedControllerMatch &&
            BetterCandidate(
                controller.priority,
                controller.order,
                selectedPriority,
                selectedOrder,
                true);
        if (betterLayer || sameLayerBetter) {
            selectedController = &controller;
            selectedControllerMatch = match;
            selectedPriority = controller.priority;
            selectedOrder = controller.order;
            hasSelected = true;
        }
    }
    if (selectedController != nullptr) {
        ApplyProfilePatch(profile, selectedController->patch);
        profile.controllerMatch = selectedControllerMatch;
        profile.controllerPatchApplied = HasProfilePatch(selectedController->patch);
        std::memcpy(
            profile.controllerAlias,
            selectedController->alias,
            std::strlen(selectedController->alias) + 1);
    }
#ifdef BSCN_SINGLE_DLL
    if (bondone::IsStadia(identity.vendor, identity.name)) {
        profile.nativeInput.blockDirectInput = true;
        profile.controllerPatchApplied = true;
        std::memcpy(profile.controllerAlias, "Stadia", sizeof("Stadia"));
    }
#endif
    return profile;
}

const ControllerProfileConfig* SelectDirectInputController(
    const RuntimeConfig& config,
    const DirectInputDeviceIdentity& identity,
    ControllerMatchKind& selectedMatch) noexcept {
    const ControllerProfileConfig* selected = nullptr;
    selectedMatch = ControllerMatchKind::None;
    std::int32_t priority = 0;
    std::uint32_t order = 0;
    bool hasSelected = false;
    for (const ControllerProfileConfig& controller : config.controllers) {
        const ControllerMatchKind match =
            EvaluateDirectInputControllerMatch(controller, identity);
        if (match == ControllerMatchKind::None) continue;
        const bool betterLayer = !hasSelected ||
            ControllerMatchRank(match) < ControllerMatchRank(selectedMatch);
        const bool sameLayerBetter = hasSelected && match == selectedMatch &&
            BetterCandidate(
                controller.priority,
                controller.order,
                priority,
                order,
                true);
        if (betterLayer || sameLayerBetter) {
            selected = &controller;
            selectedMatch = match;
            priority = controller.priority;
            order = controller.order;
            hasSelected = true;
        }
    }
    return selected;
}

void ApplyNativeInputDecision(
    DirectInputPolicyDecision& decision,
    const NativeInputPatch& patch) noexcept {
    if (patch.blockDirectInputConfigured) {
        decision.blockDirectInput = patch.blockDirectInput;
    }
    if (patch.traceDirectInputConfigured) {
        decision.traceDirectInput = patch.traceDirectInput;
    }
}

bool ConfigRequestsDirectInputInterception(
    const RuntimeConfig& config) noexcept {
#ifdef BSCN_SINGLE_DLL
    // 无线设备可在游戏启动后才接入，必须一直保留 DirectInput 受控拦截。
    return config.configValid;
#else
    // 默认保留原生 DirectInput；只有显式打开过滤或跟踪时才建立包装层。
    // 即使当前没有可隔离设备，也保留代理以支持游戏运行中的热插拔。
    if (!config.configValid) return false;
    bool enabled = false;
    const NativeInputPatch& project = config.project.patch.nativeInput;
    if (project.blockDirectInputConfigured) enabled = project.blockDirectInput;
    if (enabled || (project.traceDirectInputConfigured && project.traceDirectInput)) return true;
    for (const ControllerProfileConfig& controller : config.controllers) {
        if (!controller.enabled) continue;
        const NativeInputPatch& patch = controller.patch.nativeInput;
        if ((patch.blockDirectInputConfigured && patch.blockDirectInput) ||
            (patch.traceDirectInputConfigured && patch.traceDirectInput)) return true;
    }
    return false;
#endif
}

DirectInputPolicyDecision ResolveDirectInputPolicy(
    const RuntimeConfig& config,
    const DirectInputDeviceIdentity& identity) noexcept {
    DirectInputPolicyDecision decision{};
    if (!config.configValid) return decision;

    ApplyNativeInputDecision(decision, config.project.patch.nativeInput);
    decision.projectPatchApplied =
        config.project.patch.nativeInput.blockDirectInputConfigured ||
        config.project.patch.nativeInput.traceDirectInputConfigured;

    ControllerMatchKind controllerMatch = ControllerMatchKind::None;
    const ControllerProfileConfig* controller =
        SelectDirectInputController(config, identity, controllerMatch);
    if (controller != nullptr) {
        decision.controllerMatch = controllerMatch;
        ApplyNativeInputDecision(decision, controller->patch.nativeInput);
        decision.controllerPatchApplied =
            controller->patch.nativeInput.blockDirectInputConfigured ||
            controller->patch.nativeInput.traceDirectInputConfigured;
        std::memcpy(
            decision.controllerAlias,
            controller->alias,
            std::strlen(controller->alias) + 1);
    }
#ifdef BSCN_SINGLE_DLL
    decision.blockDirectInput = bondone::IsStadia(identity.vendor, identity.name);
    decision.traceDirectInput = false;
    decision.controllerPatchApplied = decision.blockDirectInput;
    if (decision.blockDirectInput) {
        std::memcpy(decision.controllerAlias, "Stadia", sizeof("Stadia"));
    }
#endif
    return decision;
}

void WriteLogLine(const RuntimeConfig& config, const wchar_t* text) noexcept {
#ifdef BSCN_SINGLE_DLL
    if (config.enableLog && text != nullptr) bondone::WriteLog(text);
#else
    if (!config.enableLog || config.logPath[0] == L'\0' || text == nullptr) return;
    AsyncLogItem* item = static_cast<AsyncLogItem*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(AsyncLogItem)));
    if (item == nullptr) return;
    const std::size_t pathLength = std::wcslen(config.logPath);
    const std::size_t copyLength = pathLength < (MAX_PATH - 1) ? pathLength : (MAX_PATH - 1);
    std::wmemcpy(item->path, config.logPath, copyLength);
    item->path[copyLength] = L'\0';
    item->startupDelayMs = config.logStartupDelayMs;

    wchar_t line[1024]{};
    SYSTEMTIME time{};
    GetLocalTime(&time);
    const int count = std::swprintf(
        line,
        1024,
        L"[%04u-%02u-%02u %02u:%02u:%02u] %ls\r\n",
        static_cast<unsigned int>(time.wYear),
        static_cast<unsigned int>(time.wMonth),
        static_cast<unsigned int>(time.wDay),
        static_cast<unsigned int>(time.wHour),
        static_cast<unsigned int>(time.wMinute),
        static_cast<unsigned int>(time.wSecond),
        text);
    if (count <= 0) {
        HeapFree(GetProcessHeap(), 0, item);
        return;
    }
    const int bytes = WideCharToMultiByte(
        CP_UTF8,
        0,
        line,
        count,
        item->utf8,
        static_cast<int>(sizeof(item->utf8)),
        nullptr,
        nullptr);
    if (bytes <= 0) {
        HeapFree(GetProcessHeap(), 0, item);
        return;
    }
    item->bytes = static_cast<DWORD>(bytes);
    const LONG64 proposedNotBefore = static_cast<LONG64>(GetTickCount64() + config.logStartupDelayMs);
    InterlockedCompareExchange64(&g_logNotBeforeTick, proposedNotBefore, 0);
    if (!QueueUserWorkItem(AsyncLogWorker, item, WT_EXECUTEDEFAULT)) {
        HeapFree(GetProcessHeap(), 0, item);
    }
#endif
}

}  // namespace joyshim
