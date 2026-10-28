#include "managed_devices.hpp"

#include <windows.h>

namespace joyshim {
namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
std::array<ManagedDeviceIdentity, MaxXInputUsers> g_devices{};
std::size_t g_count{};

}  // namespace

void PublishManagedDevices(
    const std::array<ManagedDeviceIdentity, MaxXInputUsers>& devices,
    std::size_t count) noexcept {
    // 整体复制后再更新数量，读者不会看到只更新了一半的设备集合。
    AcquireSRWLockExclusive(&g_lock);
    g_devices = devices;
    g_count = count <= g_devices.size() ? count : g_devices.size();
    ReleaseSRWLockExclusive(&g_lock);
}

bool IsManagedDirectInputDevice(
    std::uint16_t vendor, std::uint16_t product, const wchar_t* path) noexcept {
    if (path == nullptr || !*path) return false;
    char utf8Path[MaxManagedDevicePath]{};
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1,
                            utf8Path, static_cast<int>(sizeof(utf8Path)),
                            nullptr, nullptr) == 0) return false;
    AcquireSRWLockShared(&g_lock);
    const bool result = IsUniquelyManagedDevice(
        g_devices.data(), g_count, vendor, product, utf8Path);
    ReleaseSRWLockShared(&g_lock);
    return result;
}


#ifdef BSCN_SINGLE_DLL
bool IsManagedStadiaDirectInputDevice(
    std::uint16_t vendor, std::uint16_t product, const wchar_t* path) noexcept {
    if (path == nullptr || !*path) return false;
    char utf8Path[MaxManagedDevicePath]{};
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1,
                            utf8Path, static_cast<int>(sizeof(utf8Path)),
                            nullptr, nullptr) == 0) return false;
    AcquireSRWLockShared(&g_lock);
    const bool result = IsUniquelyManagedStadia(
        g_devices.data(), g_count, vendor, product, utf8Path);
    ReleaseSRWLockShared(&g_lock);
    return result;
}
#endif

}  // namespace joyshim
