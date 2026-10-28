#include "controller_match.hpp"

#include <cctype>
#include <cstring>

namespace joyshim {
namespace {

char LowerAscii(char value) noexcept {
    if (value >= 'A' && value <= 'Z') return static_cast<char>(value - 'A' + 'a');
    return value;
}

bool EqualsAsciiIgnoreCase(const char* left, const char* right) noexcept {
    if (left == nullptr || right == nullptr) return false;
    while (*left != '\0' && *right != '\0') {
        if (LowerAscii(*left) != LowerAscii(*right)) return false;
        ++left;
        ++right;
    }
    return *left == *right;
}

bool ContainsAsciiIgnoreCase(const char* text, const char* fragment) noexcept {
    if (text == nullptr || fragment == nullptr || fragment[0] == '\0') return false;
    const std::size_t textLength = std::strlen(text);
    const std::size_t fragmentLength = std::strlen(fragment);
    if (fragmentLength > textLength) return false;
    for (std::size_t start = 0; start + fragmentLength <= textLength; ++start) {
        bool equal = true;
        for (std::size_t index = 0; index < fragmentLength; ++index) {
            if (LowerAscii(text[start + index]) != LowerAscii(fragment[index])) {
                equal = false;
                break;
            }
        }
        if (equal) return true;
    }
    return false;
}

bool AnyExactTextMatch(
    const std::vector<DeviceTextValue>& values,
    const char* actual) noexcept {
    for (const DeviceTextValue& value : values) {
        if (EqualsAsciiIgnoreCase(value.value, actual)) return true;
    }
    return false;
}

bool AnyExactPathMatch(
    const std::vector<DevicePathValue>& values,
    const char* actual) noexcept {
    for (const DevicePathValue& value : values) {
        if (EqualsAsciiIgnoreCase(value.value, actual)) return true;
    }
    return false;
}

bool AnyContainsTextMatch(
    const std::vector<DeviceTextValue>& values,
    const char* actual) noexcept {
    for (const DeviceTextValue& value : values) {
        if (ContainsAsciiIgnoreCase(actual, value.value)) return true;
    }
    return false;
}

bool AnyDeviceIdMatch(
    const std::vector<DeviceIdValue>& values,
    std::uint16_t vendor,
    std::uint16_t product) noexcept {
    for (const DeviceIdValue& value : values) {
        if (value.vendor == vendor && value.product == product) return true;
    }
    return false;
}

bool AnyGuidMatch(
    const std::vector<std::array<char, 33>>& values,
    const char* actual) noexcept {
    for (const auto& value : values) {
        if (EqualsAsciiIgnoreCase(value.data(), actual)) return true;
    }
    return false;
}

bool AnyConnectionMatch(
    const std::vector<ConnectionKind>& values,
    ConnectionKind actual) noexcept {
    for (ConnectionKind value : values) {
        if (value == actual) return true;
    }
    return false;
}

}  // namespace

ControllerMatchKind EvaluateControllerMatch(
    const ControllerMatchFields& controller,
    const DeviceIdentity& identity) noexcept {
    if (!controller.enabled) return ControllerMatchKind::None;

    // Connections 是身份确认后的限定条件，不参与身份优先级。配置了连接方式
    // 但当前连接不符合时，该定义整体不成立，也不会退回到 GUID。
    if (!controller.connections.empty() &&
        !AnyConnectionMatch(controller.connections, identity.connection)) {
        return ControllerMatchKind::None;
    }

    // 严格分层回退：越靠前的信息越接近用户实际想表达的设备范围。
    // GUID 只在所有更可靠字段均未命中时作为最后一条识别线。
    if (!controller.serials.empty() &&
        AnyExactTextMatch(controller.serials, identity.serial)) {
        return ControllerMatchKind::Serial;
    }
    if (!controller.paths.empty() &&
        AnyExactPathMatch(controller.paths, identity.path)) {
        return ControllerMatchKind::Path;
    }
    if (!controller.deviceIds.empty() &&
        AnyDeviceIdMatch(controller.deviceIds, identity.vendor, identity.product)) {
        return ControllerMatchKind::DeviceId;
    }
    if (!controller.names.empty() &&
        AnyExactTextMatch(controller.names, identity.name)) {
        return ControllerMatchKind::Name;
    }
    if (!controller.nameContains.empty() &&
        AnyContainsTextMatch(controller.nameContains, identity.name)) {
        return ControllerMatchKind::NameContains;
    }
    if (!controller.guids.empty() && AnyGuidMatch(controller.guids, identity.guid)) {
        return ControllerMatchKind::GuidFallback;
    }
    return controller.matchAny ? ControllerMatchKind::MatchAny : ControllerMatchKind::None;
}

ControllerMatchKind EvaluateDirectInputControllerMatch(
    const ControllerMatchFields& controller,
    const DirectInputDeviceIdentity& identity) noexcept {
    if (!controller.enabled) return ControllerMatchKind::None;

    // DirectInput 枚举没有 SDL 序列号、设备路径、连接方式和 SDL GUID。
    // 这里只使用它确实提供的型号级信息；绝不把 SDL GUID 反解成 VID/PID
    // 来伪造 GUID 命中。
    if (!controller.connections.empty()) return ControllerMatchKind::None;
    if (!controller.deviceIds.empty() &&
        AnyDeviceIdMatch(controller.deviceIds, identity.vendor, identity.product)) {
        return ControllerMatchKind::DeviceId;
    }
    if (!controller.names.empty() &&
        AnyExactTextMatch(controller.names, identity.name)) {
        return ControllerMatchKind::Name;
    }
    if (!controller.nameContains.empty() &&
        AnyContainsTextMatch(controller.nameContains, identity.name)) {
        return ControllerMatchKind::NameContains;
    }
    return controller.matchAny ? ControllerMatchKind::MatchAny : ControllerMatchKind::None;
}

const wchar_t* ControllerMatchKindName(ControllerMatchKind kind) noexcept {
    switch (kind) {
        case ControllerMatchKind::Serial: return L"Serials";
        case ControllerMatchKind::Path: return L"Paths";
        case ControllerMatchKind::DeviceId: return L"DeviceIds";
        case ControllerMatchKind::Name: return L"Names";
        case ControllerMatchKind::NameContains: return L"NameContains";
        case ControllerMatchKind::GuidFallback: return L"GuidsFallback";
        case ControllerMatchKind::MatchAny: return L"MatchAny";
        case ControllerMatchKind::None:
        default: return L"None";
    }
}

}  // namespace joyshim
