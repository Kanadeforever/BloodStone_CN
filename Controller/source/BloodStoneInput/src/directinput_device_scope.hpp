#pragma once

#include <cstdint>

namespace joyshim {

// DirectInput 8 DIDEVICEINSTANCE.dwDevType: 低字节是 DI8DEVTYPE_xxx。
// 只隐藏可映射为 XInput 手柄的 joystick/gamepad/第一人称控制器。
// 键鼠及明确分类为飞行/驾驶设备的外设不会被隐藏。
// 注意：驱动如把飞行杆报告成通用 JOYSTICK，仍会命中此过滤。
// 设备以 DI8DEVTYPE_DEVICE 报告时，仅认可 HID 通用桌面页的游戏杆/手柄。
inline constexpr bool IsDirectInputGameController(
    std::uint32_t deviceType,
    std::uint16_t usagePage,
    std::uint16_t usage) noexcept {
    const std::uint32_t type = deviceType & 0xffu;
    if (type == 0x14u || type == 0x15u || type == 0x18u) return true;
    return type == 0x11u && usagePage == 0x01u &&
        (usage == 0x04u || usage == 0x05u);
}

}  // namespace joyshim
