#include "../../src/bloodstone_device_family.hpp"
#include "../../src/managed_device_scope.hpp"
#include <cassert>
int main() {
    // 有线识别、无线名称和设备厂商三条路径都可识别 Stadia。
    assert(bondone::IsStadia(0x18D1, "Google Stadia Controller"));
    assert(bondone::IsStadia(0x0000, "Stadia (Bluetooth)"));
    assert(bondone::IsStadia(0x18D1, "Wireless Controller"));
    // 非 Stadia 的 Xbox、PS、Nintendo 必须保持原生 DirectInput 可见。
    assert(!bondone::IsStadia(0x045E, "Xbox Controller"));
    assert(!bondone::IsStadia(0x054C, "DualSense"));
    assert(!bondone::IsStadia(0x057E, "Nintendo Switch Pro Controller"));
#ifdef BSCN_SINGLE_DLL
    joyshim::ManagedDeviceIdentity devices[2]{};
    devices[0].vendor = 0x18d1;
    devices[0].product = 0x9400;
    devices[0].stadia = true;
    std::strcpy(devices[0].path, R"(\\?\hid#stadia-wired)");
    devices[1].vendor = 0x045e;
    devices[1].product = 0x02ff;
    devices[1].stadia = false;
    std::strcpy(devices[1].path, R"(\\?\hid#xbox)");
    assert(joyshim::IsUniquelyManagedStadia(devices, 2, 0x18d1, 0x9400, devices[0].path));
    assert(!joyshim::IsUniquelyManagedStadia(devices, 2, 0x045e, 0x02ff, devices[1].path));
    assert(!joyshim::IsUniquelyManagedStadia(devices, 2, 0x18d1, 0x9400, devices[1].path));
#endif
}
