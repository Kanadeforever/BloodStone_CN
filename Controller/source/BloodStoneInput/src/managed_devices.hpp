#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "managed_device_scope.hpp"
#include "slot_assignment.hpp"

namespace joyshim {

// 输入线程在发布状态之后原子替换该快照。DirectInput 线程只读取快照，
// 不调用 SDL API，因此不会与 SDL 设备枚举或设备回调相互等待。
void PublishManagedDevices(
    const std::array<ManagedDeviceIdentity, MaxXInputUsers>& devices,
    std::size_t count) noexcept;

// 此查询仅说明 SDL 已接管并发布了指定设备，不代表整个系统都无法访问它。
// DirectInput 代理还必须验证设备类别、TOML 开关和调用所在路径。
bool IsManagedDirectInputDevice(
    std::uint16_t vendor, std::uint16_t product, const wchar_t* devicePath) noexcept;

#ifdef BSCN_SINGLE_DLL
// DirectInput 线程按 SDL 实际接管设备的身份判断 Stadia，有线/蓝牙均适用。
bool IsManagedStadiaDirectInputDevice(
    std::uint16_t vendor, std::uint16_t product, const wchar_t* devicePath) noexcept;
#endif

}  // namespace joyshim
