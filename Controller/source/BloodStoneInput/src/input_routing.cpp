#include "input_routing.hpp"

#include <cwchar>
#include <cwctype>
#include <iterator>

namespace joyshim {
namespace {

bool EqualsIgnoreCase(const wchar_t* left, const wchar_t* right) noexcept {
    if (left == nullptr || right == nullptr) return false;
    while (*left != L'\0' && *right != L'\0') {
        if (std::towlower(*left) != std::towlower(*right)) return false;
        ++left;
        ++right;
    }
    return *left == *right;
}

wchar_t* Trim(wchar_t* text) noexcept {
    if (text == nullptr) return nullptr;
    while (*text != L'\0' && std::iswspace(*text)) ++text;
    wchar_t* end = text + std::wcslen(text);
    while (end > text && std::iswspace(end[-1])) --end;
    *end = L'\0';
    return text;
}

bool StartsWithIgnoreCase(const wchar_t* text, const wchar_t* prefix) noexcept {
    if (text == nullptr || prefix == nullptr) return false;
    while (*prefix != L'\0') {
        if (*text == L'\0' || std::towlower(*text) != std::towlower(*prefix)) return false;
        ++text;
        ++prefix;
    }
    return true;
}

bool ParseUnsignedSuffix(
    const wchar_t* text,
    const wchar_t* prefix,
    std::uint16_t maximum,
    std::uint16_t& output) noexcept {
    const std::size_t prefixLength = std::wcslen(prefix);
    if (!StartsWithIgnoreCase(text, prefix)) return false;
    const wchar_t* number = text + prefixLength;
    if (*number == L'\0') return false;
    wchar_t* end = nullptr;
    const unsigned long parsed = std::wcstoul(number, &end, 10);
    if (end == number || *end != L'\0' || parsed > maximum) return false;
    output = static_cast<std::uint16_t>(parsed);
    return true;
}

bool ParseStandardButtonName(const wchar_t* name, StandardButton& output) noexcept {
    struct Entry { const wchar_t* name; StandardButton value; };
    static constexpr Entry entries[] = {
        {L"South", StandardButton::South},
        {L"East", StandardButton::East},
        {L"West", StandardButton::West},
        {L"North", StandardButton::North},
        {L"Back", StandardButton::Back},
        {L"Guide", StandardButton::Guide},
        {L"Start", StandardButton::Start},
        {L"LeftStick", StandardButton::LeftStick},
        {L"RightStick", StandardButton::RightStick},
        {L"LeftShoulder", StandardButton::LeftShoulder},
        {L"RightShoulder", StandardButton::RightShoulder},
        {L"DpadUp", StandardButton::DpadUp},
        {L"DpadDown", StandardButton::DpadDown},
        {L"DpadLeft", StandardButton::DpadLeft},
        {L"DpadRight", StandardButton::DpadRight},
        {L"Misc1", StandardButton::Misc1},
        {L"RightPaddle1", StandardButton::RightPaddle1},
        {L"LeftPaddle1", StandardButton::LeftPaddle1},
        {L"RightPaddle2", StandardButton::RightPaddle2},
        {L"LeftPaddle2", StandardButton::LeftPaddle2},
        {L"Touchpad", StandardButton::Touchpad},
        {L"Misc2", StandardButton::Misc2},
        {L"Misc3", StandardButton::Misc3},
        {L"Misc4", StandardButton::Misc4},
        {L"Misc5", StandardButton::Misc5},
        {L"Misc6", StandardButton::Misc6},
    };
    for (const Entry& entry : entries) {
        if (EqualsIgnoreCase(name, entry.name)) {
            output = entry.value;
            return true;
        }
    }
    return false;
}

bool ParseStandardAxisName(const wchar_t* name, StandardAxis& output) noexcept {
    struct Entry { const wchar_t* name; StandardAxis value; };
    static constexpr Entry entries[] = {
        {L"LeftX", StandardAxis::LeftX},
        {L"LeftY", StandardAxis::LeftY},
        {L"RightX", StandardAxis::RightX},
        {L"RightY", StandardAxis::RightY},
        {L"LeftTrigger", StandardAxis::LeftTrigger},
        {L"RightTrigger", StandardAxis::RightTrigger},
    };
    for (const Entry& entry : entries) {
        if (EqualsIgnoreCase(name, entry.name)) {
            output = entry.value;
            return true;
        }
    }
    return false;
}

bool ParseSingleButtonSource(const wchar_t* text, ButtonSource& output) noexcept {
    if (EqualsIgnoreCase(text, L"None")) {
        output = {};
        return true;
    }
    constexpr const wchar_t gamepadPrefix[] = L"Gamepad.";
    if (StartsWithIgnoreCase(text, gamepadPrefix)) {
        StandardButton button{};
        if (!ParseStandardButtonName(text + std::size(gamepadPrefix) - 1, button)) return false;
        output = StandardButtonSource(button);
        return true;
    }
    std::uint16_t rawIndex{};
    if (ParseUnsignedSuffix(text, L"Raw.Button", 255u, rawIndex)) {
        output = ButtonSource{ButtonSourceKind::Raw, rawIndex};
        return true;
    }
    return false;
}

}  // namespace

InputRouting MakeDefaultInputRouting() noexcept {
    InputRouting routing{};
    const auto set = [&](XInputButtonTarget target, StandardButton source) noexcept {
        ButtonBinding& binding = routing.buttons[ToIndex(target)];
        binding.sources[0] = StandardButtonSource(source);
        binding.count = 1;
    };
    set(XInputButtonTarget::DpadUp, StandardButton::DpadUp);
    set(XInputButtonTarget::DpadDown, StandardButton::DpadDown);
    set(XInputButtonTarget::DpadLeft, StandardButton::DpadLeft);
    set(XInputButtonTarget::DpadRight, StandardButton::DpadRight);
    set(XInputButtonTarget::Start, StandardButton::Start);
    set(XInputButtonTarget::Back, StandardButton::Back);
    set(XInputButtonTarget::LeftThumb, StandardButton::LeftStick);
    set(XInputButtonTarget::RightThumb, StandardButton::RightStick);
    set(XInputButtonTarget::LeftShoulder, StandardButton::LeftShoulder);
    set(XInputButtonTarget::RightShoulder, StandardButton::RightShoulder);
    set(XInputButtonTarget::Guide, StandardButton::Guide);
    set(XInputButtonTarget::A, StandardButton::South);
    set(XInputButtonTarget::B, StandardButton::East);
    set(XInputButtonTarget::X, StandardButton::West);
    set(XInputButtonTarget::Y, StandardButton::North);

    // 默认摇杆直接使用 SDL3 标准完整轴。SDL3 的 Y 正方向向下，
    // XInput 的 Y 正方向向上，因此两条 Y 轴在默认路由中显式反转。
    // 四半轴只保留为旧配置兼容路径，不再参与默认摇杆状态合成。
    routing.axes.leftStickX = StandardSignedAxis(StandardAxis::LeftX);
    routing.axes.leftStickY = StandardSignedAxis(StandardAxis::LeftY, true);
    routing.axes.rightStickX = StandardSignedAxis(StandardAxis::RightX);
    routing.axes.rightStickY = StandardSignedAxis(StandardAxis::RightY, true);
    routing.axes.leftTrigger = StandardHalfAxis(StandardAxis::LeftTrigger, AxisPolarity::Positive);
    routing.axes.rightTrigger = StandardHalfAxis(StandardAxis::RightTrigger, AxisPolarity::Positive);
    return routing;
}

bool ParseButtonBinding(const wchar_t* text, ButtonBinding& output) noexcept {
    if (text == nullptr) return false;
    wchar_t buffer[512]{};
    const std::size_t length = std::wcslen(text);
    if (length >= std::size(buffer)) return false;
    std::wmemcpy(buffer, text, length + 1);

    ButtonBinding parsed{};
    wchar_t* token = buffer;
    for (;;) {
        wchar_t* separator = std::wcschr(token, L'|');
        if (separator != nullptr) *separator = L'\0';
        token = Trim(token);
        if (*token == L'\0' || parsed.count >= MaxButtonSources) return false;
        ButtonSource source{};
        if (!ParseSingleButtonSource(token, source)) return false;
        if (source.kind == ButtonSourceKind::None) {
            if (parsed.count != 0 || separator != nullptr) return false;
            output = {};
            return true;
        }
        parsed.sources[parsed.count++] = source;
        if (separator == nullptr) break;
        token = separator + 1;
    }
    if (parsed.count == 0) return false;
    output = parsed;
    return true;
}

bool ParseHalfAxisSource(const wchar_t* text, HalfAxisSource& output) noexcept {
    if (text == nullptr) return false;
    wchar_t buffer[256]{};
    const std::size_t length = std::wcslen(text);
    if (length >= std::size(buffer)) return false;
    std::wmemcpy(buffer, text, length + 1);
    wchar_t* value = Trim(buffer);
    if (EqualsIgnoreCase(value, L"None")) {
        output = {};
        return true;
    }

    wchar_t* lastDot = std::wcsrchr(value, L'.');
    if (lastDot == nullptr) return false;
    *lastDot = L'\0';
    const wchar_t* polarityText = lastDot + 1;
    AxisPolarity polarity{};
    if (EqualsIgnoreCase(polarityText, L"Negative")) {
        polarity = AxisPolarity::Negative;
    } else if (EqualsIgnoreCase(polarityText, L"Positive")) {
        polarity = AxisPolarity::Positive;
    } else if (EqualsIgnoreCase(polarityText, L"Full")) {
        polarity = AxisPolarity::Full;
    } else if (EqualsIgnoreCase(polarityText, L"FullInverted")) {
        polarity = AxisPolarity::FullInverted;
    } else {
        return false;
    }

    constexpr const wchar_t gamepadPrefix[] = L"Gamepad.";
    if (StartsWithIgnoreCase(value, gamepadPrefix)) {
        StandardAxis axis{};
        if (!ParseStandardAxisName(value + std::size(gamepadPrefix) - 1, axis)) return false;
        output = StandardHalfAxis(axis, polarity);
        return true;
    }

    std::uint16_t rawIndex{};
    if (ParseUnsignedSuffix(value, L"Raw.Axis", 255u, rawIndex)) {
        output = HalfAxisSource{AxisSourceKind::Raw, rawIndex, polarity};
        return true;
    }
    return false;
}

bool ParseSignedAxisSource(const wchar_t* text, SignedAxisSource& output) noexcept {
    if (text == nullptr) return false;
    wchar_t buffer[256]{};
    const std::size_t length = std::wcslen(text);
    if (length >= std::size(buffer)) return false;
    std::wmemcpy(buffer, text, length + 1);
    wchar_t* value = Trim(buffer);

    if (EqualsIgnoreCase(value, L"None")) {
        output = {};
        return true;
    }

    bool inverted = false;
    constexpr const wchar_t invertedSuffix[] = L".Inverted";
    const std::size_t valueLength = std::wcslen(value);
    const std::size_t suffixLength = std::size(invertedSuffix) - 1;
    if (valueLength > suffixLength &&
        EqualsIgnoreCase(value + valueLength - suffixLength, invertedSuffix)) {
        value[valueLength - suffixLength] = L'\0';
        inverted = true;
    }

    constexpr const wchar_t gamepadPrefix[] = L"Gamepad.";
    if (StartsWithIgnoreCase(value, gamepadPrefix)) {
        StandardAxis axis{};
        if (!ParseStandardAxisName(value + std::size(gamepadPrefix) - 1, axis)) return false;
        output = SignedAxisSource{
            AxisSourceKind::Standard,
            static_cast<std::uint16_t>(axis),
            inverted,
        };
        return true;
    }

    std::uint16_t rawIndex{};
    if (ParseUnsignedSuffix(value, L"Raw.Axis", 255u, rawIndex)) {
        output = SignedAxisSource{AxisSourceKind::Raw, rawIndex, inverted};
        return true;
    }
    return false;
}

void FormatSignedAxisSource(
    const SignedAxisSource& source,
    wchar_t* output,
    std::size_t outputCount) noexcept {
    if (output == nullptr || outputCount == 0) return;
    output[0] = L'\0';
    if (source.kind == AxisSourceKind::None) {
        std::swprintf(output, outputCount, L"None");
        return;
    }

    const wchar_t* suffix = source.inverted ? L".Inverted" : L"";
    if (source.kind == AxisSourceKind::Raw) {
        std::swprintf(
            output,
            outputCount,
            L"Raw.Axis%u%ls",
            static_cast<unsigned int>(source.index),
            suffix);
        return;
    }

    const wchar_t* axisName = L"Invalid";
    switch (static_cast<StandardAxis>(source.index)) {
        case StandardAxis::LeftX: axisName = L"LeftX"; break;
        case StandardAxis::LeftY: axisName = L"LeftY"; break;
        case StandardAxis::RightX: axisName = L"RightX"; break;
        case StandardAxis::RightY: axisName = L"RightY"; break;
        case StandardAxis::LeftTrigger: axisName = L"LeftTrigger"; break;
        case StandardAxis::RightTrigger: axisName = L"RightTrigger"; break;
        default: break;
    }
    std::swprintf(output, outputCount, L"Gamepad.%ls%ls", axisName, suffix);
}

void FormatHalfAxisSource(
    const HalfAxisSource& source,
    wchar_t* output,
    std::size_t outputCount) noexcept {
    if (output == nullptr || outputCount == 0) return;
    output[0] = L'\0';
    if (source.kind == AxisSourceKind::None) {
        std::swprintf(output, outputCount, L"None");
        return;
    }

    const wchar_t* polarity = L"Positive";
    switch (source.polarity) {
        case AxisPolarity::Negative: polarity = L"Negative"; break;
        case AxisPolarity::Positive: polarity = L"Positive"; break;
        case AxisPolarity::Full: polarity = L"Full"; break;
        case AxisPolarity::FullInverted: polarity = L"FullInverted"; break;
    }

    if (source.kind == AxisSourceKind::Raw) {
        std::swprintf(
            output,
            outputCount,
            L"Raw.Axis%u.%ls",
            static_cast<unsigned int>(source.index),
            polarity);
        return;
    }

    const wchar_t* axisName = L"Invalid";
    switch (static_cast<StandardAxis>(source.index)) {
        case StandardAxis::LeftX: axisName = L"LeftX"; break;
        case StandardAxis::LeftY: axisName = L"LeftY"; break;
        case StandardAxis::RightX: axisName = L"RightX"; break;
        case StandardAxis::RightY: axisName = L"RightY"; break;
        case StandardAxis::LeftTrigger: axisName = L"LeftTrigger"; break;
        case StandardAxis::RightTrigger: axisName = L"RightTrigger"; break;
        default: break;
    }
    std::swprintf(output, outputCount, L"Gamepad.%ls.%ls", axisName, polarity);
}


}  // namespace joyshim
