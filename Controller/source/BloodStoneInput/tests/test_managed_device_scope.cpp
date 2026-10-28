#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include "managed_device_scope.hpp"

int main() {
    using namespace joyshim;
    ManagedDeviceIdentity devices[2]{};
    devices[0].vendor = 0x18d1;
    devices[0].product = 0x9400;
    std::strcpy(devices[0].path, R"(\\?\hid#vid_18d1&pid_9400#device_one)");
    // 只有确实已发布的实例才可以隐藏，VID/PID 相同但实例不同不应隐藏。
    assert(IsUniquelyManagedDevice(devices, 1, 0x18d1, 0x9400,
        R"(\\?\HID#VID_18D1&PID_9400#DEVICE_ONE)"));
    assert(!IsUniquelyManagedDevice(devices, 1, 0x18d1, 0x9400,
        R"(\\?\hid#vid_18d1&pid_9400#device_two)"));
    assert(!IsUniquelyManagedDevice(devices, 1, 0, 0x9400, devices[0].path));
    assert(!IsUniquelyManagedDevice(devices, 1, 0x18d1, 0, devices[0].path));
    assert(!IsUniquelyManagedDevice(devices, 1, 0x18d1, 0x9400, ""));
    assert(!IsUniquelyManagedDevice(nullptr, 0, 0x18d1, 0x9400, devices[0].path));
    devices[1] = devices[0];
    assert(!IsUniquelyManagedDevice(devices, 2, 0x18d1, 0x9400, devices[0].path));
    return 0;
}
