#ifdef NDEBUG
#undef NDEBUG
#endif

#include <array>
#include <cassert>
#include <cstdint>
#include <cwchar>

#include "input_routing.hpp"

int main() {
    using namespace joyshim;

    static_assert(HalfAxisMagnitude(-32768, AxisPolarity::Negative) == 32768u);
    static_assert(HalfAxisMagnitude(-1234, AxisPolarity::Negative) == 1234u);
    static_assert(HalfAxisMagnitude(1234, AxisPolarity::Positive) == 1234u);
    static_assert(ComposeSignedAxis(32768u, 0u) == -32768);
    static_assert(ComposeSignedAxis(0u, 32767u) == 32767);
    static_assert(ConvertHalfAxisToTrigger(32768u) == 255u);
    static_assert(HalfAxisMagnitude(-32768, AxisPolarity::Full) == 0u);
    static_assert(HalfAxisMagnitude(32767, AxisPolarity::Full) == 32767u);
    static_assert(HalfAxisMagnitude(-32768, AxisPolarity::FullInverted) == 32767u);
    static_assert(HalfAxisMagnitude(32767, AxisPolarity::FullInverted) == 0u);

    ButtonBinding buttonBinding{};
    assert(ParseButtonBinding(L"Gamepad.Back|Gamepad.Touchpad", buttonBinding));
    assert(buttonBinding.count == 2);
    assert(buttonBinding.sources[0].kind == ButtonSourceKind::Standard);
    assert(buttonBinding.sources[1].index == static_cast<std::uint16_t>(StandardButton::Touchpad));
    assert(ParseButtonBinding(L"Raw.Button17", buttonBinding));
    assert(buttonBinding.count == 1 && buttonBinding.sources[0].index == 17);
    assert(ParseButtonBinding(L"None", buttonBinding));
    assert(buttonBinding.count == 0);
    assert(!ParseButtonBinding(L"Gamepad.Unknown", buttonBinding));

    // 覆盖 SDL3 当前公开的全部标准 Gamepad 按键名称，防止新增映射项
    // 只写入文档而没有进入解析器。
    static constexpr const wchar_t* allStandardButtons[] = {
        L"Gamepad.South", L"Gamepad.East", L"Gamepad.West", L"Gamepad.North",
        L"Gamepad.Back", L"Gamepad.Guide", L"Gamepad.Start",
        L"Gamepad.LeftStick", L"Gamepad.RightStick",
        L"Gamepad.LeftShoulder", L"Gamepad.RightShoulder",
        L"Gamepad.DpadUp", L"Gamepad.DpadDown", L"Gamepad.DpadLeft", L"Gamepad.DpadRight",
        L"Gamepad.Misc1", L"Gamepad.RightPaddle1", L"Gamepad.LeftPaddle1",
        L"Gamepad.RightPaddle2", L"Gamepad.LeftPaddle2", L"Gamepad.Touchpad",
        L"Gamepad.Misc2", L"Gamepad.Misc3", L"Gamepad.Misc4", L"Gamepad.Misc5", L"Gamepad.Misc6",
    };
    for (const wchar_t* name : allStandardButtons) {
        assert(ParseButtonBinding(name, buttonBinding));
        assert(buttonBinding.count == 1);
        assert(buttonBinding.sources[0].kind == ButtonSourceKind::Standard);
    }

    // 15 个 XInput 输出位必须互不冲突并覆盖 Guide/Xbox 非公开状态位。
    std::uint16_t allTargetMasks = 0;
    for (std::size_t index = 0; index < XInputButtonTargetCount; ++index) {
        const std::uint16_t mask = ButtonMaskForTarget(static_cast<XInputButtonTarget>(index));
        assert(mask != 0);
        assert((allTargetMasks & mask) == 0);
        allTargetMasks = static_cast<std::uint16_t>(allTargetMasks | mask);
    }
    assert((allTargetMasks & Guide) != 0);
    assert((allTargetMasks & A) != 0);
    assert((allTargetMasks & Y) != 0);

    HalfAxisSource axisSource{};
    assert(ParseHalfAxisSource(L"Gamepad.RightY.Negative", axisSource));
    assert(axisSource.kind == AxisSourceKind::Standard);
    assert(axisSource.index == static_cast<std::uint16_t>(StandardAxis::RightY));
    assert(axisSource.polarity == AxisPolarity::Negative);
    assert(ParseHalfAxisSource(L"Raw.Axis4.Positive", axisSource));
    assert(axisSource.kind == AxisSourceKind::Raw && axisSource.index == 4);
    assert(ParseHalfAxisSource(L"Raw.Axis2.Full", axisSource));
    assert(axisSource.polarity == AxisPolarity::Full);
    assert(ParseHalfAxisSource(L"Raw.Axis2.FullInverted", axisSource));
    assert(axisSource.polarity == AxisPolarity::FullInverted);
    assert(!ParseHalfAxisSource(L"Raw.Axis4.Sideways", axisSource));

    SignedAxisSource signedSource{};
    assert(ParseSignedAxisSource(L"Gamepad.RightX", signedSource));
    assert(signedSource.kind == AxisSourceKind::Standard);
    assert(signedSource.index == static_cast<std::uint16_t>(StandardAxis::RightX));
    assert(!signedSource.inverted);
    assert(ParseSignedAxisSource(L"Raw.Axis3.Inverted", signedSource));
    assert(signedSource.kind == AxisSourceKind::Raw && signedSource.index == 3);
    assert(signedSource.inverted);
    assert(ParseSignedAxisSource(L"None", signedSource));
    assert(signedSource.kind == AxisSourceKind::None);
    assert(!ParseSignedAxisSource(L"Raw.Axis3.Negative", signedSource));

    InputRouting routing = MakeDefaultInputRouting();
    std::array<bool, static_cast<std::size_t>(StandardButton::Count)> buttons{};
    std::array<std::int16_t, static_cast<std::size_t>(StandardAxis::Count)> axes{};
    buttons[static_cast<std::size_t>(StandardButton::South)] = true;
    buttons[static_cast<std::size_t>(StandardButton::Touchpad)] = true;
    buttons[static_cast<std::size_t>(StandardButton::Guide)] = true;
    axes[static_cast<std::size_t>(StandardAxis::RightX)] = -20000;
    axes[static_cast<std::size_t>(StandardAxis::RightY)] = 12000;
    axes[static_cast<std::size_t>(StandardAxis::LeftTrigger)] = 32767;

    const auto standardState = EvaluateInputRouting(
        routing,
        [&](const ButtonSource& source) noexcept {
            return source.kind == ButtonSourceKind::Standard &&
                source.index < buttons.size() && buttons[source.index];
        },
        [&](const HalfAxisSource& source) noexcept -> std::int16_t {
            return source.kind == AxisSourceKind::Standard && source.index < axes.size()
                ? axes[source.index] : 0;
        });
    assert((standardState.buttons & A) != 0);
    assert((standardState.buttons & Back) == 0);  // 默认不再把 Touchpad 改写为 Back。
    assert((standardState.buttons & Guide) != 0);
    assert(standardState.rightX == -20000);
    assert(standardState.rightY == -12000);
    assert(standardState.leftTrigger == 255);

    // 验证所有输出都可以重定向或禁用，并且原始轴的正负半轴独立合成。
    assert(ParseButtonBinding(
        L"Gamepad.North", routing.buttons[ToIndex(XInputButtonTarget::A)]));
    assert(ParseButtonBinding(
        L"Gamepad.Misc1", routing.buttons[ToIndex(XInputButtonTarget::Guide)]));
    assert(ParseButtonBinding(
        L"None", routing.buttons[ToIndex(XInputButtonTarget::B)]));
    routing.axes.rightStickX = {};
    routing.axes.rightStickY = {};
    assert(ParseHalfAxisSource(L"Raw.Axis3.Negative", routing.axes.rightStickLeft));
    assert(ParseHalfAxisSource(L"Raw.Axis3.Positive", routing.axes.rightStickRight));
    assert(ParseHalfAxisSource(L"Raw.Axis4.Negative", routing.axes.rightStickUp));
    assert(ParseHalfAxisSource(L"Raw.Axis4.Positive", routing.axes.rightStickDown));

    std::array<bool, 32> rawButtons{};
    std::array<std::int16_t, 8> rawAxes{};
    buttons.fill(false);
    buttons[static_cast<std::size_t>(StandardButton::North)] = true;
    buttons[static_cast<std::size_t>(StandardButton::Misc1)] = true;
    rawAxes[3] = -25000;
    rawAxes[4] = 18000;
    const auto remappedState = EvaluateInputRouting(
        routing,
        [&](const ButtonSource& source) noexcept {
            if (source.kind == ButtonSourceKind::Standard) {
                return source.index < buttons.size() && buttons[source.index];
            }
            return source.kind == ButtonSourceKind::Raw &&
                source.index < rawButtons.size() && rawButtons[source.index];
        },
        [&](const HalfAxisSource& source) noexcept -> std::int16_t {
            if (source.kind == AxisSourceKind::Standard) {
                return source.index < axes.size() ? axes[source.index] : 0;
            }
            return source.kind == AxisSourceKind::Raw && source.index < rawAxes.size()
                ? rawAxes[source.index] : 0;
        });
    assert((remappedState.buttons & A) != 0);
    assert((remappedState.buttons & Guide) != 0);
    assert((remappedState.buttons & B) == 0);
    assert(remappedState.rightX == -25000);
    assert(remappedState.rightY == -18000);


    // 六条标准逻辑轴必须保持 SDL3 官方顺序，尤其从 RightX 开始不能与
    // RightY/LeftTrigger/RightTrigger 串位。每条轴使用不同哨兵值。
    axes.fill(0);
    axes[static_cast<std::size_t>(StandardAxis::LeftX)] = 1011;
    axes[static_cast<std::size_t>(StandardAxis::LeftY)] = 2022;
    axes[static_cast<std::size_t>(StandardAxis::RightX)] = 3033;
    axes[static_cast<std::size_t>(StandardAxis::RightY)] = 4044;
    axes[static_cast<std::size_t>(StandardAxis::LeftTrigger)] = 5055;
    axes[static_cast<std::size_t>(StandardAxis::RightTrigger)] = 6066;
    const auto sentinelState = EvaluateInputRouting(
        MakeDefaultInputRouting(),
        [](const ButtonSource&) noexcept { return false; },
        [&](const HalfAxisSource& source) noexcept -> std::int16_t {
            return source.kind == AxisSourceKind::Standard && source.index < axes.size()
                ? axes[source.index] : 0;
        });
    assert(sentinelState.leftX == 1011);
    assert(sentinelState.leftY == -2022);
    assert(sentinelState.rightX == 3033);
    assert(sentinelState.rightY == -4044);
    assert(sentinelState.leftTrigger == ConvertHalfAxisToTrigger(5055));
    assert(sentinelState.rightTrigger == ConvertHalfAxisToTrigger(6066));

    // 验证一种常见原始轴布局：a2/a3 为右摇杆，a4/a5 为扳机。
    InputRouting stadiaRaw = MakeDefaultInputRouting();
    stadiaRaw.axes.rightStickX = {};
    stadiaRaw.axes.rightStickY = {};
    assert(ParseHalfAxisSource(L"Raw.Axis2.Negative", stadiaRaw.axes.rightStickLeft));
    assert(ParseHalfAxisSource(L"Raw.Axis2.Positive", stadiaRaw.axes.rightStickRight));
    assert(ParseHalfAxisSource(L"Raw.Axis3.Negative", stadiaRaw.axes.rightStickUp));
    assert(ParseHalfAxisSource(L"Raw.Axis3.Positive", stadiaRaw.axes.rightStickDown));
    assert(ParseHalfAxisSource(L"Raw.Axis4.Full", stadiaRaw.axes.leftTrigger));
    assert(ParseHalfAxisSource(L"Raw.Axis5.Full", stadiaRaw.axes.rightTrigger));
    std::array<std::int16_t, 6> stadiaAxes{0, 0, 31476, 3611, -32768, 32767};
    const auto stadiaState = EvaluateInputRouting(
        stadiaRaw,
        [](const ButtonSource&) noexcept { return false; },
        [&](const HalfAxisSource& source) noexcept -> std::int16_t {
            return source.kind == AxisSourceKind::Raw && source.index < stadiaAxes.size()
                ? stadiaAxes[source.index] : 0;
        });
    assert(stadiaState.rightX == 31476);
    assert(stadiaState.rightY == -3611);
    assert(stadiaState.leftTrigger == 0);
    assert(stadiaState.rightTrigger == 255);

    wchar_t formatted[96]{};
    FormatHalfAxisSource(stadiaRaw.axes.rightStickLeft, formatted, 96);
    assert(std::wcscmp(formatted, L"Raw.Axis2.Negative") == 0);

    // 直接有符号轴在异号象限必须同时保留两条轴，避免重复读取半轴
    // 导致右上/左下两个方向相互抵消为零。
    InputRouting directRawLayout = MakeDefaultInputRouting();
    assert(ParseSignedAxisSource(L"Raw.Axis2", directRawLayout.axes.rightStickX));
    assert(ParseSignedAxisSource(
        L"Raw.Axis3.Inverted", directRawLayout.axes.rightStickY));
    rawAxes.fill(0);
    rawAxes[2] = 21000;
    rawAxes[3] = -17000;
    const auto rightUp = EvaluateInputRouting(
        directRawLayout,
        [](const ButtonSource&) noexcept { return false; },
        [&](const HalfAxisSource& source) noexcept -> std::int16_t {
            return source.kind == AxisSourceKind::Raw &&
                source.index < rawAxes.size() ? rawAxes[source.index] : 0;
        });
    assert(rightUp.rightX == 21000);
    assert(rightUp.rightY == 17000);

    rawAxes[2] = -22000;
    rawAxes[3] = 16000;
    const auto leftDown = EvaluateInputRouting(
        directRawLayout,
        [](const ButtonSource&) noexcept { return false; },
        [&](const HalfAxisSource& source) noexcept -> std::int16_t {
            return source.kind == AxisSourceKind::Raw &&
                source.index < rawAxes.size() ? rawAxes[source.index] : 0;
        });
    assert(leftDown.rightX == -22000);
    assert(leftDown.rightY == -16000);

    FormatSignedAxisSource(directRawLayout.axes.rightStickY, formatted, 96);
    assert(std::wcscmp(formatted, L"Raw.Axis3.Inverted") == 0);

    // 原始按钮也必须能参与多来源 OR，且所有 XInput 输出都由同一套路由求值。
    assert(ParseButtonBinding(
        L"Raw.Button7|Raw.Button12", routing.buttons[ToIndex(XInputButtonTarget::RightShoulder)]));
    rawButtons[12] = true;
    const auto rawButtonState = EvaluateInputRouting(
        routing,
        [&](const ButtonSource& source) noexcept {
            if (source.kind == ButtonSourceKind::Standard) {
                return source.index < buttons.size() && buttons[source.index];
            }
            return source.kind == ButtonSourceKind::Raw &&
                source.index < rawButtons.size() && rawButtons[source.index];
        },
        [&](const HalfAxisSource& source) noexcept -> std::int16_t {
            if (source.kind == AxisSourceKind::Standard) {
                return source.index < axes.size() ? axes[source.index] : 0;
            }
            return source.kind == AxisSourceKind::Raw && source.index < rawAxes.size()
                ? rawAxes[source.index] : 0;
        });
    assert((rawButtonState.buttons & RightShoulder) != 0);
    return 0;
}
