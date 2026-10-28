#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <cstddef>
#include <cstring>

#include "config.hpp"

namespace {

template <std::size_t Size>
void CopyText(char (&destination)[Size], const char* source) noexcept {
    static_assert(Size > 0, "目标字符数组不能为空");
    destination[0] = '\0';
    if (source == nullptr) return;

    const std::size_t length = std::strlen(source);
    const std::size_t count = length < Size - 1 ? length : Size - 1;
    if (count != 0) std::memcpy(destination, source, count);
    destination[count] = '\0';
}

joyshim::DeviceIdentity MakeIdentity(
    std::uint16_t vendor,
    std::uint16_t product,
    const char* name) noexcept {
    joyshim::DeviceIdentity identity{};
    identity.vendor = vendor;
    identity.product = product;
    CopyText(identity.name, name);
    return identity;
}

joyshim::DirectInputDeviceIdentity MakeDirectInputIdentity(
    std::uint16_t vendor,
    std::uint16_t product,
    const char* name) noexcept {
    joyshim::DirectInputDeviceIdentity identity{};
    identity.vendor = vendor;
    identity.product = product;
    CopyText(identity.name, name);
    return identity;
}

}  // namespace

int main() {
    using namespace joyshim;

    // 无配置时默认保留原生设备；只有配置了过滤或跟踪才启用包装。
    RuntimeConfig defaults{};
    defaults.configValid = true;
    assert(defaults.project.patch.nativeInput.blockDirectInputConfigured == false);
    assert(!ConfigRequestsDirectInputInterception(defaults));
    const DirectInputPolicyDecision defaultDecision = ResolveDirectInputPolicy(
        defaults, MakeDirectInputIdentity(0x18D1, 0x9400, "Google Stadia Controller"));
    assert(!defaultDecision.blockDirectInput);

    RuntimeConfig config{};
    config.configValid = true;

    // 顶层功能表是当前游戏全部手柄的父集合。这里设置三个默认项，
    // 后续手柄子集合只覆盖自己明确声明的字段。
    config.project.patch.nativeInput.blockDirectInputConfigured = true;
    config.project.patch.nativeInput.blockDirectInput = true;
    config.project.patch.nativeInput.traceDirectInputConfigured = true;
    config.project.patch.nativeInput.traceDirectInput = false;
    config.project.patch.inputRight.invertYConfigured = true;
    config.project.patch.inputRight.value.invertY = true;

    ControllerProfileConfig stadia{};
    stadia.enabled = true;
    stadia.priority = 100;
    stadia.order = 0;
    CopyText(stadia.alias, "CustomStadiaLabel");
    stadia.deviceIds.push_back(DeviceIdValue{0x18D1, 0x9400});

    // 子集合只覆盖 TraceDirectInput 和 SwapXY。它没有填写
    // BlockDirectInput/InvertY，因此这两项必须继续继承顶层设置。
    stadia.patch.nativeInput.traceDirectInputConfigured = true;
    stadia.patch.nativeInput.traceDirectInput = true;
    stadia.patch.inputRight.swapConfigured = true;
    stadia.patch.inputRight.value.swapXY = true;
    config.controllers.push_back(stadia);

    // 项目父集合已请求 BlockDirectInput，因此早期 DirectInput 路由必须
    // 保留包装层；该判断不能依赖手柄是否已经被 SDL 枚举。
    assert(ConfigRequestsDirectInputInterception(config));

    const DeviceIdentity genericIdentity =
        MakeIdentity(0x1234, 0x5678, "Generic Controller");
    const DeviceProfile generic = LoadDeviceProfile(config, genericIdentity);
    assert(generic.projectPatchApplied);
    assert(!generic.controllerPatchApplied);
    assert(generic.controllerMatch == ControllerMatchKind::None);
    assert(generic.controllerAlias[0] == '\0');
    assert(generic.nativeInput.blockDirectInput);
    assert(!generic.nativeInput.traceDirectInput);
    assert(generic.correction.rightStick.invertY);
    assert(!generic.correction.rightStick.swapXY);

    const DeviceIdentity stadiaIdentity =
        MakeIdentity(0x18D1, 0x9400, "Google Stadia Controller");
    const DeviceProfile matched = LoadDeviceProfile(config, stadiaIdentity);
    assert(matched.projectPatchApplied);
    assert(matched.controllerPatchApplied);
    assert(matched.controllerMatch == ControllerMatchKind::DeviceId);
    assert(std::strcmp(matched.controllerAlias, "CustomStadiaLabel") == 0);
    assert(matched.nativeInput.blockDirectInput);   // 继承父集合。
    assert(matched.nativeInput.traceDirectInput);   // 子集合覆盖。
    assert(matched.correction.rightStick.invertY);  // 继承父集合。
    assert(matched.correction.rightStick.swapXY);   // 子集合覆盖。

    // DirectInput 使用同一套父集合 -> 子集合合成规则。
    const DirectInputPolicyDecision genericDi = ResolveDirectInputPolicy(
        config,
        MakeDirectInputIdentity(0x1234, 0x5678, "Generic Controller"));
    assert(genericDi.projectPatchApplied);
    assert(!genericDi.controllerPatchApplied);
    assert(genericDi.blockDirectInput);
    assert(!genericDi.traceDirectInput);

    const DirectInputPolicyDecision stadiaDi = ResolveDirectInputPolicy(
        config,
        MakeDirectInputIdentity(0x18D1, 0x9400, "Google Stadia Controller"));
    assert(stadiaDi.projectPatchApplied);
    assert(stadiaDi.controllerPatchApplied);
    assert(stadiaDi.controllerMatch == ControllerMatchKind::DeviceId);
    assert(std::strcmp(stadiaDi.controllerAlias, "CustomStadiaLabel") == 0);
    assert(stadiaDi.blockDirectInput);  // 子集合未填写，继续继承 true。
    assert(stadiaDi.traceDirectInput);  // 子集合明确覆盖为 true。

    // 再验证子集合可以明确覆盖父集合为 false，而不是只能启用功能。
    config.controllers[0].patch.nativeInput.blockDirectInputConfigured = true;
    config.controllers[0].patch.nativeInput.blockDirectInput = false;
    const DirectInputPolicyDecision disabledForStadia = ResolveDirectInputPolicy(
        config,
        MakeDirectInputIdentity(0x18D1, 0x9400, "Google Stadia Controller"));
    assert(!disabledForStadia.blockDirectInput);
    assert(disabledForStadia.traceDirectInput);

    RuntimeConfig transparent{};
    transparent.configValid = true;
    transparent.project.patch.nativeInput.blockDirectInputConfigured = true;
    transparent.project.patch.nativeInput.blockDirectInput = false;
    assert(!ConfigRequestsDirectInputInterception(transparent));

    ControllerProfileConfig traced{};
    traced.enabled = true;
    traced.patch.nativeInput.traceDirectInputConfigured = true;
    traced.patch.nativeInput.traceDirectInput = true;
    transparent.controllers.push_back(traced);
    assert(ConfigRequestsDirectInputInterception(transparent));

    return 0;
}
