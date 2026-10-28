#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include "directinput_device_scope.hpp"

int main() {
    using joyshim::IsDirectInputGameController;
    // 普通 gamepad、joystick、第一人称设备、HID 通用桌面设备类别。
    assert(IsDirectInputGameController(0x15, 0, 0));
    assert(IsDirectInputGameController(0x14, 0, 0));
    assert(IsDirectInputGameController(0x18, 0, 0));
    assert(IsDirectInputGameController(0x115, 0, 0)); // 高位 subtype 不影响判断。
    assert(IsDirectInputGameController(0x11, 0x01, 0x04));
    assert(IsDirectInputGameController(0x11, 0x01, 0x05));
    // 各类键鼠、驾驶/飞行设备、音量遥控、无类别设备必须可见。
    assert(!IsDirectInputGameController(0x12, 0, 0));
    assert(!IsDirectInputGameController(0x13, 0, 0));
    assert(!IsDirectInputGameController(0x16, 0, 0));
    assert(!IsDirectInputGameController(0x17, 0, 0));
    assert(!IsDirectInputGameController(0x1b, 0, 0));
    assert(!IsDirectInputGameController(0x1c, 0, 0));
    assert(!IsDirectInputGameController(0x11, 0x01, 0x02));
    assert(!IsDirectInputGameController(0x11, 0, 0));
}
