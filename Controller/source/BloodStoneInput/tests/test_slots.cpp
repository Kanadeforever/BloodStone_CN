#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include "slot_assignment.hpp"

int main() {
    using namespace joyshim;
    SlotIds slots{};
    const DeviceId first[] = {11, 22, 33, 44, 55};
    slots = ReconcileSlotIds(slots, first, 5);
    assert((slots == SlotIds{11, 22, 33, 44}));

    const DeviceId second[] = {44, 22, 55, 66};
    slots = ReconcileSlotIds(slots, second, 4);
    // 22 和 44 保持原槽；55、66 填入断开的 11、33 所在空槽。
    assert((slots == SlotIds{55, 22, 66, 44}));

    const DeviceId none[] = {0};
    slots = ReconcileSlotIds(slots, none, 0);
    assert((slots == SlotIds{}));
    return 0;
}
