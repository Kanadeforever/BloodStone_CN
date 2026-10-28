#pragma once

#include <cstddef>
#include <cstdint>

namespace joyshim {

// 一条记录对应已成功打开、已经输出到一个 XInput 槽位的物理手柄。
// path 来自 SDL_GetGamepadPath，不能使用只有型号含义的 GUID 替代它。
constexpr std::size_t MaxManagedDevicePath = 512;
struct ManagedDeviceIdentity {
    std::uint16_t vendor{};
    std::uint16_t product{};
#ifdef BSCN_SINGLE_DLL
    // 此值只说明 SDL 线程确认为 Stadia；不可把这一状态扩展到其他手柄。
    bool stadia{};
#endif
    char path[MaxManagedDevicePath]{};
};

// Windows HID 设备接口路径不区分英文大小写，正反斜杠按同一种路径分隔符处理。
// 仅比较相同的完整设备实例路径，禁止单独按 VID/PID 过滤，避免同型号
// 的两只手柄只接管其中一只时，把另一只也从游戏中隐藏。
inline bool SameDeviceInstancePath(const char* first, const char* second) noexcept {
    if (first == nullptr || second == nullptr || !*first || !*second) return false;
    for (;;) {
        unsigned char a = static_cast<unsigned char>(*first++);
        unsigned char b = static_cast<unsigned char>(*second++);
        if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b + ('a' - 'A'));
        if (a == '/') a = '\\';
        if (b == '/') b = '\\';
        if (a != b) return false;
        if (a == 0) return true;
    }
}

// 不接受身份不明的设备；SDL 与 DirectInput 的路径、厂商和产品号都要
// 一致，并且只能匹配当前一条已发布记录。碰撞、缺失信息均保持可见。
inline bool IsUniquelyManagedDevice(
    const ManagedDeviceIdentity* devices, std::size_t count,
    std::uint16_t vendor, std::uint16_t product,
    const char* directInputPath) noexcept {
    if (devices == nullptr || count == 0 || vendor == 0 || product == 0 ||
        directInputPath == nullptr || !*directInputPath) return false;
    std::size_t matches = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const auto& device = devices[index];
        if (device.vendor == vendor && device.product == product &&
            SameDeviceInstancePath(device.path, directInputPath)) ++matches;
    }
    return matches == 1;
}


#ifdef BSCN_SINGLE_DLL
// 只有与已经接管的唯一设备逐字段匹配，且 SDL 已将其识别为 Stadia，才允许隐藏。
inline bool IsUniquelyManagedStadia(
    const ManagedDeviceIdentity* devices, std::size_t count,
    std::uint16_t vendor, std::uint16_t product, const char* directInputPath) noexcept {
    if (!IsUniquelyManagedDevice(devices, count, vendor, product, directInputPath))
        return false;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& device = devices[i];
        if (device.vendor == vendor && device.product == product &&
            SameDeviceInstancePath(device.path, directInputPath)) return device.stadia;
    }
    return false;
}
#endif
}  // namespace joyshim
