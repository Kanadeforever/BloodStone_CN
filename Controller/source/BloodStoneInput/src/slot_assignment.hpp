#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace joyshim {

constexpr std::size_t MaxXInputUsers = 4;
using DeviceId = std::uint32_t;
using SlotIds = std::array<DeviceId, MaxXInputUsers>;

constexpr bool Contains(const DeviceId* ids, std::size_t count, DeviceId id) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (ids[index] == id) return true;
    }
    return false;
}

// 保留仍在线设备的原槽位，并按 SDL 枚举顺序把新设备放入最低空槽。
constexpr SlotIds ReconcileSlotIds(
    SlotIds current,
    const DeviceId* connected,
    std::size_t connectedCount) noexcept {
    for (DeviceId& id : current) {
        if (id != 0 && !Contains(connected, connectedCount, id)) id = 0;
    }

    for (std::size_t device = 0; device < connectedCount; ++device) {
        const DeviceId id = connected[device];
        if (id == 0 || Contains(current.data(), current.size(), id)) continue;
        for (DeviceId& slot : current) {
            if (slot == 0) {
                slot = id;
                break;
            }
        }
    }
    return current;
}

}  // namespace joyshim
