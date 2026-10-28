#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "input_mapping.hpp"

namespace joyshim {

// 与 SDL_GamepadButton 的稳定公共枚举顺序一致，但不依赖 SDL3 头文件。
enum class StandardButton : std::uint8_t {
    South = 0,
    East,
    West,
    North,
    Back,
    Guide,
    Start,
    LeftStick,
    RightStick,
    LeftShoulder,
    RightShoulder,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Misc1,
    RightPaddle1,
    LeftPaddle1,
    RightPaddle2,
    LeftPaddle2,
    Touchpad,
    Misc2,
    Misc3,
    Misc4,
    Misc5,
    Misc6,
    Count,
};

enum class StandardAxis : std::uint8_t {
    LeftX = 0,
    LeftY,
    RightX,
    RightY,
    LeftTrigger,
    RightTrigger,
    Count,
};

static_assert(static_cast<std::uint8_t>(StandardAxis::LeftX) == 0);
static_assert(static_cast<std::uint8_t>(StandardAxis::LeftY) == 1);
static_assert(static_cast<std::uint8_t>(StandardAxis::RightX) == 2);
static_assert(static_cast<std::uint8_t>(StandardAxis::RightY) == 3);
static_assert(static_cast<std::uint8_t>(StandardAxis::LeftTrigger) == 4);
static_assert(static_cast<std::uint8_t>(StandardAxis::RightTrigger) == 5);

// 使用具名字段保存 SDL3 六条标准逻辑轴，避免 RX/RY/LT/RT 因数组下标、
// 字段顺序或后续枚举扩展而发生串位。配置层仍使用 StandardAxis 标识来源，
// 实际读取始终经过显式 switch。
struct StandardAxisValues {
    std::int16_t leftX{};
    std::int16_t leftY{};
    std::int16_t rightX{};
    std::int16_t rightY{};
    std::int16_t leftTrigger{};
    std::int16_t rightTrigger{};

    constexpr std::int16_t Get(StandardAxis axis) const noexcept {
        switch (axis) {
            case StandardAxis::LeftX: return leftX;
            case StandardAxis::LeftY: return leftY;
            case StandardAxis::RightX: return rightX;
            case StandardAxis::RightY: return rightY;
            case StandardAxis::LeftTrigger: return leftTrigger;
            case StandardAxis::RightTrigger: return rightTrigger;
            default: return 0;
        }
    }
};

enum class ButtonSourceKind : std::uint8_t {
    None,
    Standard,
    Raw,
};

struct ButtonSource {
    ButtonSourceKind kind{ButtonSourceKind::None};
    std::uint16_t index{};
};

constexpr std::size_t MaxButtonSources = 8;

struct ButtonBinding {
    std::array<ButtonSource, MaxButtonSources> sources{};
    std::uint8_t count{};
};

enum class AxisSourceKind : std::uint8_t {
    None,
    Standard,
    Raw,
};

enum class AxisPolarity : std::uint8_t {
    Negative,
    Positive,
    // 将完整 -32768..32767 范围线性转换为 0..32767，主要用于
    // DirectInput 等以负极值为扳机静止位置的原始轴。
    Full,
    FullInverted,
};

struct HalfAxisSource {
    AxisSourceKind kind{AxisSourceKind::None};
    std::uint16_t index{};
    AxisPolarity polarity{AxisPolarity::Positive};
};

// 直接有符号轴来源。它不再拆成正/负两个半轴，因此同一物理轴在一次
// XInputGetState 中只参与一次求值，可避免后端状态在两次查询之间变化时
// 出现正负半轴互相抵消。Y 轴是否反转由 inverted 明确指定。
struct SignedAxisSource {
    AxisSourceKind kind{AxisSourceKind::None};
    std::uint16_t index{};
    bool inverted{};
};

enum class XInputButtonTarget : std::uint8_t {
    DpadUp = 0,
    DpadDown,
    DpadLeft,
    DpadRight,
    Start,
    Back,
    LeftThumb,
    RightThumb,
    LeftShoulder,
    RightShoulder,
    Guide,
    A,
    B,
    X,
    Y,
    Count,
};

constexpr std::size_t XInputButtonTargetCount =
    static_cast<std::size_t>(XInputButtonTarget::Count);

struct AxisRouting {
    // 直接有符号轴优先于下面的四半轴兼容路由。默认均为 None，
    // 因而旧配置行为保持不变。
    SignedAxisSource leftStickX{};
    SignedAxisSource leftStickY{};
    SignedAxisSource rightStickX{};
    SignedAxisSource rightStickY{};

    HalfAxisSource leftStickLeft{};
    HalfAxisSource leftStickRight{};
    HalfAxisSource leftStickUp{};
    HalfAxisSource leftStickDown{};
    HalfAxisSource rightStickLeft{};
    HalfAxisSource rightStickRight{};
    HalfAxisSource rightStickUp{};
    HalfAxisSource rightStickDown{};
    HalfAxisSource leftTrigger{};
    HalfAxisSource rightTrigger{};
};

struct InputRouting {
    std::array<ButtonBinding, XInputButtonTargetCount> buttons{};
    AxisRouting axes{};
    bool customized{};
    std::uint16_t invalidEntries{};
};

struct RoutedState {
    std::uint16_t buttons{};
    std::uint8_t leftTrigger{};
    std::uint8_t rightTrigger{};
    std::int16_t leftX{};
    std::int16_t leftY{};
    std::int16_t rightX{};
    std::int16_t rightY{};
};

constexpr std::size_t ToIndex(XInputButtonTarget target) noexcept {
    return static_cast<std::size_t>(target);
}

constexpr std::uint16_t ButtonMaskForTarget(XInputButtonTarget target) noexcept {
    switch (target) {
        case XInputButtonTarget::DpadUp: return DpadUp;
        case XInputButtonTarget::DpadDown: return DpadDown;
        case XInputButtonTarget::DpadLeft: return DpadLeft;
        case XInputButtonTarget::DpadRight: return DpadRight;
        case XInputButtonTarget::Start: return Start;
        case XInputButtonTarget::Back: return Back;
        case XInputButtonTarget::LeftThumb: return LeftThumb;
        case XInputButtonTarget::RightThumb: return RightThumb;
        case XInputButtonTarget::LeftShoulder: return LeftShoulder;
        case XInputButtonTarget::RightShoulder: return RightShoulder;
        case XInputButtonTarget::Guide: return Guide;
        case XInputButtonTarget::A: return A;
        case XInputButtonTarget::B: return B;
        case XInputButtonTarget::X: return X;
        case XInputButtonTarget::Y: return Y;
        default: return 0;
    }
}

constexpr ButtonSource StandardButtonSource(StandardButton button) noexcept {
    return ButtonSource{
        ButtonSourceKind::Standard,
        static_cast<std::uint16_t>(button),
    };
}

constexpr SignedAxisSource StandardSignedAxis(
    StandardAxis axis,
    bool inverted = false) noexcept {
    return SignedAxisSource{
        AxisSourceKind::Standard,
        static_cast<std::uint16_t>(axis),
        inverted,
    };
}

constexpr HalfAxisSource StandardHalfAxis(
    StandardAxis axis,
    AxisPolarity polarity) noexcept {
    return HalfAxisSource{
        AxisSourceKind::Standard,
        static_cast<std::uint16_t>(axis),
        polarity,
    };
}

InputRouting MakeDefaultInputRouting() noexcept;

// 解析按键来源。多个来源使用“|”连接，任意一个按下即激活目标按键。
// 示例：Gamepad.Back|Gamepad.Touchpad、Raw.Button12、None。
bool ParseButtonBinding(const wchar_t* text, ButtonBinding& output) noexcept;

// 解析轴来源。示例：Gamepad.RightX.Negative、Raw.Axis4.Positive、Raw.Axis2.Full、None。
bool ParseHalfAxisSource(const wchar_t* text, HalfAxisSource& output) noexcept;

// 解析直接有符号轴。示例：Gamepad.RightX、Gamepad.RightY.Inverted、
// Raw.Axis2、Raw.Axis3.Inverted、None。
bool ParseSignedAxisSource(const wchar_t* text, SignedAxisSource& output) noexcept;

// 将解析后的轴来源还原为稳定、可复制到 TOML 的文本，供日志和诊断工具显示。
void FormatSignedAxisSource(
    const SignedAxisSource& source,
    wchar_t* output,
    std::size_t outputCount) noexcept;

void FormatHalfAxisSource(
    const HalfAxisSource& source,
    wchar_t* output,
    std::size_t outputCount) noexcept;

constexpr std::uint16_t HalfAxisMagnitude(
    std::int16_t value,
    AxisPolarity polarity) noexcept {
    if (polarity == AxisPolarity::Positive) {
        return value > 0 ? static_cast<std::uint16_t>(value) : 0u;
    }
    if (polarity == AxisPolarity::Negative) {
        if (value >= 0) return 0u;
        return value == static_cast<std::int16_t>(-32768)
            ? 32768u
            : static_cast<std::uint16_t>(-value);
    }

    const std::uint32_t shifted = static_cast<std::uint32_t>(
        static_cast<std::int32_t>(value) + 32768);
    const std::uint16_t full = static_cast<std::uint16_t>(
        (shifted * 32767u + 32767u) / 65535u);
    return polarity == AxisPolarity::Full ? full
        : static_cast<std::uint16_t>(32767u - full);
}

constexpr std::int16_t ComposeSignedAxis(
    std::uint16_t negativeMagnitude,
    std::uint16_t positiveMagnitude) noexcept {
    std::int32_t result = static_cast<std::int32_t>(positiveMagnitude) -
        static_cast<std::int32_t>(negativeMagnitude);
    if (result < -32768) result = -32768;
    if (result > 32767) result = 32767;
    return static_cast<std::int16_t>(result);
}

constexpr std::uint8_t ConvertHalfAxisToTrigger(std::uint16_t magnitude) noexcept {
    const std::uint32_t clamped = magnitude > 32767u ? 32767u : magnitude;
    return static_cast<std::uint8_t>((clamped * 255u + 16383u) / 32767u);
}

constexpr std::int16_t InvertSignedAxis(std::int16_t value) noexcept {
    return value == static_cast<std::int16_t>(-32768)
        ? static_cast<std::int16_t>(32767)
        : static_cast<std::int16_t>(-value);
}

template <typename ButtonReader, typename AxisReader>
RoutedState EvaluateInputRouting(
    const InputRouting& routing,
    ButtonReader&& readButton,
    AxisReader&& readAxis) noexcept {
    RoutedState state{};
    for (std::size_t targetIndex = 0; targetIndex < XInputButtonTargetCount; ++targetIndex) {
        const ButtonBinding& binding = routing.buttons[targetIndex];
        bool pressed = false;
        for (std::size_t sourceIndex = 0; sourceIndex < binding.count; ++sourceIndex) {
            if (readButton(binding.sources[sourceIndex])) {
                pressed = true;
                break;
            }
        }
        if (pressed) {
            state.buttons |= ButtonMaskForTarget(
                static_cast<XInputButtonTarget>(targetIndex));
        }
    }

    const auto magnitude = [&](const HalfAxisSource& source) noexcept {
        if (source.kind == AxisSourceKind::None) return static_cast<std::uint16_t>(0);
        return HalfAxisMagnitude(readAxis(source), source.polarity);
    };

    const auto signedAxis = [&](const SignedAxisSource& source) noexcept {
        if (source.kind == AxisSourceKind::None) return static_cast<std::int16_t>(0);
        const HalfAxisSource query{source.kind, source.index, AxisPolarity::Positive};
        const std::int16_t value = readAxis(query);
        return source.inverted ? InvertSignedAxis(value) : value;
    };

    state.leftX = routing.axes.leftStickX.kind != AxisSourceKind::None
        ? signedAxis(routing.axes.leftStickX)
        : ComposeSignedAxis(
            magnitude(routing.axes.leftStickLeft),
            magnitude(routing.axes.leftStickRight));
    // XInput 的 Y 正方向向上。直接路由需显式使用 .Inverted；旧四半轴
    // 路由继续把 Down 作为负方向、Up 作为正方向。
    state.leftY = routing.axes.leftStickY.kind != AxisSourceKind::None
        ? signedAxis(routing.axes.leftStickY)
        : ComposeSignedAxis(
            magnitude(routing.axes.leftStickDown),
            magnitude(routing.axes.leftStickUp));
    state.rightX = routing.axes.rightStickX.kind != AxisSourceKind::None
        ? signedAxis(routing.axes.rightStickX)
        : ComposeSignedAxis(
            magnitude(routing.axes.rightStickLeft),
            magnitude(routing.axes.rightStickRight));
    state.rightY = routing.axes.rightStickY.kind != AxisSourceKind::None
        ? signedAxis(routing.axes.rightStickY)
        : ComposeSignedAxis(
            magnitude(routing.axes.rightStickDown),
            magnitude(routing.axes.rightStickUp));
    state.leftTrigger = ConvertHalfAxisToTrigger(magnitude(routing.axes.leftTrigger));
    state.rightTrigger = ConvertHalfAxisToTrigger(magnitude(routing.axes.rightTrigger));
    return state;
}

}  // namespace joyshim
