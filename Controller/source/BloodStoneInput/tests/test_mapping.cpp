#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include "input_mapping.hpp"

int main() {
    using namespace joyshim;
    static_assert(ConvertStick(12345, false) == 12345);
    static_assert(ConvertStick(12345, true) == -12345);
    static_assert(ConvertStick(static_cast<std::int16_t>(-32768), true) == 32767);
    // 验证 SDL 标准轴到 XInput 的四个方向保持独立，不会把 RX 复制到 RY。
    static_assert(ConvertStick(-20000, false) == -20000);  // 左
    static_assert(ConvertStick(20000, false) == 20000);    // 右
    static_assert(ConvertStick(-20000, true) == 20000);    // SDL 上 -> XInput 上
    static_assert(ConvertStick(20000, true) == -20000);    // SDL 下 -> XInput 下
    static_assert(ConvertTrigger(-1) == 0);
    static_assert(ConvertTrigger(0) == 0);
    static_assert(ConvertTrigger(16384) == 128);
    static_assert(ConvertTrigger(32767) == 255);

    Buttons all{};
    all.south = all.east = all.west = all.north = true;
    all.back = all.touchpad = all.guide = all.start = true;
    all.leftStick = all.rightStick = true;
    all.leftShoulder = all.rightShoulder = true;
    all.dpadUp = all.dpadDown = all.dpadLeft = all.dpadRight = true;
    const auto normal = BuildButtonMask(all, false);
    const auto extended = BuildButtonMask(all, true);
    assert((normal & Guide) == 0);
    assert((extended & Guide) != 0);
    assert((normal & Back) != 0);
    assert((normal & A) && (normal & B) && (normal & X) && (normal & Y));

    Buttons touchpadOnly{};
    touchpadOnly.touchpad = true;
    assert(BuildButtonMask(touchpadOnly, false) == Back);
    return 0;
}
