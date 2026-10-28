#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <cstddef>
#include <cstring>

#include "controller_match.hpp"

namespace {

template <std::size_t Size>
void CopyText(char (&destination)[Size], const char* text) noexcept {
    static_assert(Size > 0, "目标字符数组不能为空");
    destination[0] = '\0';
    if (text == nullptr) return;

    const std::size_t length = std::strlen(text);
    const std::size_t count = length < Size - 1 ? length : Size - 1;
    if (count != 0) std::memcpy(destination, text, count);
    destination[count] = '\0';
}

template <typename Value>
Value TextValue(const char* text) {
    Value value{};
    if (text != nullptr) {
        const std::size_t length = std::strlen(text);
        const std::size_t count = length < sizeof(value.value) - 1
            ? length : sizeof(value.value) - 1;
        if (count != 0) std::memcpy(value.value, text, count);
        value.value[count] = '\0';
    }
    return value;
}

std::array<char, 33> GuidValue(const char* text) {
    std::array<char, 33> value{};
    if (text != nullptr) {
        const std::size_t length = std::strlen(text);
        const std::size_t count = length < value.size() - 1
            ? length : value.size() - 1;
        if (count != 0) std::memcpy(value.data(), text, count);
        value[count] = '\0';
    }
    return value;
}

}  // namespace

int main() {
    using namespace joyshim;

    ControllerMatchFields controller{};
    controller.enabled = true;
    controller.serials.push_back(TextValue<DeviceTextValue>("OTHER-SERIAL"));
    controller.paths.push_back(TextValue<DevicePathValue>("\\\\?\\HID#OTHER"));
    controller.deviceIds.push_back(DeviceIdValue{0x18D1, 0x9400});
    controller.names.push_back(TextValue<DeviceTextValue>("Google Stadia Controller"));
    controller.nameContains.push_back(TextValue<DeviceTextValue>("Stadia"));
    controller.guids.push_back(GuidValue("11111111111111111111111111111111"));
    controller.guids.push_back(GuidValue("030068bad11800000094000000016800"));

    DeviceIdentity identity{};
    identity.vendor = 0x18D1;
    identity.product = 0x9400;
    CopyText(identity.guid, "030068bad11800000094000000016800");
    CopyText(identity.name, "Google Stadia Controller");
    CopyText(identity.serial, "REAL-SERIAL");
    CopyText(identity.path, "\\\\?\\HID#REAL");

    // 即使 GUID 也匹配，DeviceIds 必须更早结束识别。
    assert(EvaluateControllerMatch(controller, identity) ==
        ControllerMatchKind::DeviceId);

    controller.deviceIds.clear();
    // 完整名称必须先于名称片段和 GUID。
    assert(EvaluateControllerMatch(controller, identity) ==
        ControllerMatchKind::Name);

    controller.names.clear();
    assert(EvaluateControllerMatch(controller, identity) ==
        ControllerMatchKind::NameContains);

    controller.nameContains.clear();
    // Guids 支持多个值，并且只有前面的识别线都失败后才生效。
    assert(EvaluateControllerMatch(controller, identity) ==
        ControllerMatchKind::GuidFallback);

    controller.guids.clear();
    controller.matchAny = true;
    assert(EvaluateControllerMatch(controller, identity) ==
        ControllerMatchKind::MatchAny);

    controller.connections = {ConnectionKind::Wireless};
    identity.connection = ConnectionKind::Wired;
    assert(EvaluateControllerMatch(controller, identity) ==
        ControllerMatchKind::None);

    ControllerMatchFields directInput{};
    directInput.enabled = true;
    directInput.serials.push_back(TextValue<DeviceTextValue>("UNAVAILABLE"));
    directInput.guids.push_back(GuidValue("030068bad11800000094000000016800"));
    directInput.deviceIds.push_back(DeviceIdValue{0x18D1, 0x9400});
    DirectInputDeviceIdentity diIdentity{};
    diIdentity.vendor = 0x18D1;
    diIdentity.product = 0x9400;
    CopyText(diIdentity.name, "Google Stadia Controller");
    assert(EvaluateDirectInputControllerMatch(directInput, diIdentity) ==
        ControllerMatchKind::DeviceId);

    directInput.deviceIds.clear();
    // DirectInput 不得伪造 SDL GUID 命中。
    assert(EvaluateDirectInputControllerMatch(directInput, diIdentity) ==
        ControllerMatchKind::None);

    return 0;
}
