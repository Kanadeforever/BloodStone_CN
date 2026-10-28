#pragma once

#include <cstdint>

namespace joyshim {

// 与 XInput 的 XINPUT_GAMEPAD_* 位值完全一致。
constexpr std::uint16_t DpadUp = 0x0001;
constexpr std::uint16_t DpadDown = 0x0002;
constexpr std::uint16_t DpadLeft = 0x0004;
constexpr std::uint16_t DpadRight = 0x0008;
constexpr std::uint16_t Start = 0x0010;
constexpr std::uint16_t Back = 0x0020;
constexpr std::uint16_t LeftThumb = 0x0040;
constexpr std::uint16_t RightThumb = 0x0080;
constexpr std::uint16_t LeftShoulder = 0x0100;
constexpr std::uint16_t RightShoulder = 0x0200;
constexpr std::uint16_t Guide = 0x0400;
constexpr std::uint16_t A = 0x1000;
constexpr std::uint16_t B = 0x2000;
constexpr std::uint16_t X = 0x4000;
constexpr std::uint16_t Y = 0x8000;

struct Buttons {
    bool south{};
    bool east{};
    bool west{};
    bool north{};
    bool back{};
    bool touchpad{};
    bool guide{};
    bool start{};
    bool leftStick{};
    bool rightStick{};
    bool leftShoulder{};
    bool rightShoulder{};
    bool dpadUp{};
    bool dpadDown{};
    bool dpadLeft{};
    bool dpadRight{};
};

constexpr std::uint16_t BuildButtonMask(const Buttons& input, bool includeGuide) noexcept {
    std::uint16_t result = 0;
    if (input.dpadUp) result |= DpadUp;
    if (input.dpadDown) result |= DpadDown;
    if (input.dpadLeft) result |= DpadLeft;
    if (input.dpadRight) result |= DpadRight;
    if (input.start) result |= Start;
    // DualShock 4 / DualSense 的触摸板机械按下按约定作为 XInput Back。
    if (input.back || input.touchpad) result |= Back;
    if (input.leftStick) result |= LeftThumb;
    if (input.rightStick) result |= RightThumb;
    if (input.leftShoulder) result |= LeftShoulder;
    if (input.rightShoulder) result |= RightShoulder;
    if (includeGuide && input.guide) result |= Guide;
    if (input.south) result |= A;
    if (input.east) result |= B;
    if (input.west) result |= X;
    if (input.north) result |= Y;
    return result;
}

// SDL3 和 XInput 的摇杆值域相同。SDL3 的 Y 正方向向下，XInput 向上。
constexpr std::int16_t ConvertStick(std::int16_t value, bool invert) noexcept {
    if (!invert) return value;
    return value == static_cast<std::int16_t>(-32768)
        ? static_cast<std::int16_t>(32767)
        : static_cast<std::int16_t>(-value);
}

// SDL3 扳机为 0..32767，XInput 为 0..255。
constexpr std::uint8_t ConvertTrigger(std::int16_t value) noexcept {
    if (value <= 0) return 0;
    const std::uint32_t positive = static_cast<std::uint16_t>(value);
    return static_cast<std::uint8_t>((positive * 255u + 16383u) / 32767u);
}

}  // namespace joyshim
