#include "directinput_filter.hpp"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <new>

#include "config.hpp"
#include "directinput_device_scope.hpp"
#include "managed_devices.hpp"
#include "proxy_entry.hpp"
#include "proxy_system.hpp"
#include "win32_proc.hpp"

namespace joyshim {
namespace {

static_assert(DI8DEVTYPE_DEVICE == 0x11u, "DirectInput 8 设备类型常量不匹配");
static_assert(DI8DEVTYPE_JOYSTICK == 0x14u, "DirectInput 8 设备类型常量不匹配");
static_assert(DI8DEVTYPE_GAMEPAD == 0x15u, "DirectInput 8 设备类型常量不匹配");
static_assert(DI8DEVTYPE_1STPERSON == 0x18u, "DirectInput 8 设备类型常量不匹配");


constexpr GUID kIidIUnknown = {
    0x00000000u, 0x0000u, 0x0000u,
    {0xC0u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x46u}};
constexpr GUID kIidDirectInput8A = {
    0xBF798030u, 0x483Au, 0x4DA2u,
    {0xAAu, 0x99u, 0x5Du, 0x64u, 0xEDu, 0x36u, 0x97u, 0x00u}};
constexpr GUID kIidDirectInput8W = {
    0xBF798031u, 0x483Au, 0x4DA2u,
    {0xAAu, 0x99u, 0x5Du, 0x64u, 0xEDu, 0x36u, 0x97u, 0x00u}};

bool SameGuid(REFGUID left, REFGUID right) noexcept {
    return std::memcmp(&left, &right, sizeof(GUID)) == 0;
}

void CopyUtf8(const char* source, char* target, std::size_t capacity) noexcept {
    if (target == nullptr || capacity == 0) return;
    target[0] = '\0';
    if (source == nullptr) return;
    const std::size_t length = std::strlen(source);
    const std::size_t count = length < capacity - 1 ? length : capacity - 1;
    if (count != 0) std::memcpy(target, source, count);
    target[count] = '\0';
}

void WideToUtf8(const wchar_t* source, char* target, std::size_t capacity) noexcept {
    if (target == nullptr || capacity == 0) return;
    target[0] = '\0';
    if (source == nullptr || source[0] == L'\0') return;
    const int bytes = WideCharToMultiByte(
        CP_UTF8,
        0,
        source,
        -1,
        target,
        static_cast<int>(capacity),
        nullptr,
        nullptr);
    if (bytes <= 0) target[0] = '\0';
}

void Utf8ToWide(const char* source, wchar_t* target, std::size_t capacity) noexcept {
    if (target == nullptr || capacity == 0) return;
    target[0] = L'\0';
    if (source == nullptr || source[0] == '\0') return;
    const int count = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        source,
        -1,
        target,
        static_cast<int>(capacity));
    if (count <= 0) target[0] = L'\0';
}

DirectInputDeviceIdentity MakeIdentity(
    const GUID& productGuid,
    const char* productName) noexcept {
    DirectInputDeviceIdentity identity{};
    identity.vendor = static_cast<std::uint16_t>(productGuid.Data1 & 0xFFFFu);
    identity.product = static_cast<std::uint16_t>((productGuid.Data1 >> 16) & 0xFFFFu);
    CopyUtf8(productName, identity.name, std::size(identity.name));
    return identity;
}

DirectInputDeviceIdentity MakeIdentity(
    const GUID& productGuid,
    const wchar_t* productName) noexcept {
    DirectInputDeviceIdentity identity{};
    identity.vendor = static_cast<std::uint16_t>(productGuid.Data1 & 0xFFFFu);
    identity.product = static_cast<std::uint16_t>((productGuid.Data1 >> 16) & 0xFFFFu);
    WideToUtf8(productName, identity.name, std::size(identity.name));
    return identity;
}

// DirectInput 返回的 wchar_t 路径位于固定大小数组中。必须先确认终止符，
// 不能直接使用 wcslen 越过驱动返回的数据边界。
std::size_t BoundedWideLength(const wchar_t* text, std::size_t capacity) noexcept {
    if (text == nullptr) return capacity;
    std::size_t length = 0;
    while (length < capacity && text[length] != L'\0') ++length;
    return length;
}

class FilterState {
public:
    explicit FilterState(HMODULE module) noexcept
        : module_(module), config_(LoadRuntimeConfig(module)) {}

    void Refresh() noexcept {
        config_ = ReloadRuntimeConfig(module_, config_);
    }

    bool ShouldBlock(
        const GUID& instanceGuid,
        const DirectInputDeviceIdentity& identity,
        std::uint32_t deviceType,
        std::uint16_t usagePage,
        std::uint16_t usage,
        const wchar_t* devicePath) noexcept {
        // 必须先验证设备类别。即使项目层启用隐藏，也不能屏蔽键盘/鼠标。
        if (!IsDirectInputGameController(deviceType, usagePage, usage)) return false;
        DirectInputPolicyDecision decision =
            ResolveDirectInputPolicy(config_, identity);
        // 缺少可靠实例路径或 SDL 未发布对应槽位时必须放行。
        // 必须能记录并再次保护此 GUID，才允许从枚举结果中隐藏。
        // 否则游戏若直接按 GUID 创建设备，可能绕过枚举过滤而产生双输入。
#ifdef BSCN_SINGLE_DLL
        // DirectInput 无线设备名和 VID 可变化；先确认 SDL 当前接管的同一设备
        // 是 Stadia，且路径/VID/PID 唯一对应。其他设备一律不隐藏。
        const bool matched = IsManagedStadiaDirectInputDevice(
            identity.vendor, identity.product, devicePath);
        decision.blockDirectInput = matched;
#else
        const bool matched = decision.blockDirectInput &&
            IsManagedDirectInputDevice(identity.vendor, identity.product, devicePath);
#endif
        bool newlyBlocked = false;
        const bool hide = matched &&
            RememberBlocked(instanceGuid, identity, devicePath, newlyBlocked);
        if (decision.traceDirectInput || decision.blockDirectInput) {
            LogDecision(instanceGuid, identity, decision, hide);
        }
        if (newlyBlocked) ProxyEntryNotifyDirectInputDeviceHidden();
        return hide;
    }

    bool IsBlockedInstance(REFGUID instanceGuid) noexcept {
        // 曾经隐藏过的设备在 SDL 断开或槽位释放后必须重新变为可见；
        // 不能把历史 GUID 永久保存在屏蔽清单里。
        BlockedEntry found{};
        bool exists = false;
        AcquireSRWLockShared(&lock_);
        for (std::size_t index = 0; index < blockedCount_; ++index) {
            if (SameGuid(blocked_[index].guid, instanceGuid)) {
                found = blocked_[index];
                exists = true;
                break;
            }
        }
        ReleaseSRWLockShared(&lock_);
        if (!exists) return false;
        // 历史上已过滤的 GUID 必须重新服从当前配置；用户将开关改为
        // false 后，下一次刷新配置就不能继续按旧 GUID 阻断设备创建。
#ifndef BSCN_SINGLE_DLL
        const DirectInputPolicyDecision decision =
            ResolveDirectInputPolicy(config_, found.identity);
#endif
#ifdef BSCN_SINGLE_DLL
        // 已记录的 GUID 也必须再次确认目前确实是已接管的 Stadia。
        return IsManagedStadiaDirectInputDevice(
            found.identity.vendor, found.identity.product, found.path);
#else
        return decision.blockDirectInput && IsManagedDirectInputDevice(
            found.identity.vendor, found.identity.product, found.path);
#endif
    }

private:
    bool WasReported(REFGUID instanceGuid) noexcept {
        for (std::size_t index = 0; index < reportedCount_; ++index) {
            if (SameGuid(reported_[index], instanceGuid)) return true;
        }
        if (reportedCount_ < reported_.size()) {
            reported_[reportedCount_++] = instanceGuid;
        }
        return false;
    }

    bool RememberBlocked(
        REFGUID instanceGuid,
        const DirectInputDeviceIdentity& identity,
        const wchar_t* devicePath,
        bool& newlyBlocked) noexcept {
        newlyBlocked = false;
        if (devicePath == nullptr) return false;
        const std::size_t length = BoundedWideLength(devicePath, MaxManagedDevicePath);
        if (length == 0 || length >= MaxManagedDevicePath) return false;
        AcquireSRWLockExclusive(&lock_);
        for (std::size_t index = 0; index < blockedCount_; ++index) {
            if (SameGuid(blocked_[index].guid, instanceGuid)) {
                // 同一 GUID 再次枚举时仍应隐藏，不应重复增加统计。
                // 若实例已变化，则替换该 GUID 对应的路径和设备身份。
                BlockedEntry& existing = blocked_[index];
                existing.identity = identity;
                std::wmemcpy(existing.path, devicePath, length + 1);
                ReleaseSRWLockExclusive(&lock_);
                return true;
            }
        }
        if (blockedCount_ == blocked_.size()) {
            // 缓存已经占满就放行，避免枚举隐藏和 GUID 创建过滤不一致。
            ReleaseSRWLockExclusive(&lock_);
            return false;
        }
        BlockedEntry& entry = blocked_[blockedCount_++];
        entry.guid = instanceGuid;
        entry.identity = identity;
        std::wmemcpy(entry.path, devicePath, length + 1);
        newlyBlocked = true;
        ReleaseSRWLockExclusive(&lock_);
        return true;
    }

    void LogDecision(
        REFGUID instanceGuid,
        const DirectInputDeviceIdentity& identity,
        const DirectInputPolicyDecision& decision,
        bool actuallyHidden) noexcept {
        AcquireSRWLockExclusive(&lock_);
        const bool duplicate = WasReported(instanceGuid);
        ReleaseSRWLockExclusive(&lock_);
        if (duplicate) return;

        wchar_t name[MaxDeviceText]{};
        Utf8ToWide(identity.name, name, std::size(name));
        // 自定义子集合名称是 UTF-8，转成宽字符以避免日志乱码。
        wchar_t alias[MaxProfileAlias]{};
        Utf8ToWide(decision.controllerAlias, alias, std::size(alias));
        wchar_t message[1024]{};
        std::swprintf(
            message,
            std::size(message),
            L"DirectInput 枚举设备：VID:PID=%04X:%04X，名称=%ls，手柄子集合=%ls，匹配依据=%ls，主项目策略=%ls，手柄覆盖=%ls，处理=%ls。",
            static_cast<unsigned int>(identity.vendor),
            static_cast<unsigned int>(identity.product),
            name[0] != L'\0' ? name : L"(未知)",
            alias[0] != L'\0' ? alias : L"(无)",
            ControllerMatchKindName(decision.controllerMatch),
            decision.projectPatchApplied ? L"已配置" : L"默认",
            decision.controllerPatchApplied ? L"已覆盖" : L"未覆盖",
            actuallyHidden ? L"已匹配已接管实例并隐藏" :
                (decision.blockDirectInput ? L"实例未匹配，安全放行" : L"按配置保持可见"));
        WriteLogLine(config_, message);
    }

    HMODULE module_{};
    RuntimeConfig config_{};
    SRWLOCK lock_ = SRWLOCK_INIT;
    struct BlockedEntry {
        GUID guid{};
        DirectInputDeviceIdentity identity{};
        wchar_t path[MaxManagedDevicePath]{};
    };
    std::array<BlockedEntry, 32> blocked_{};
    std::array<GUID, 64> reported_{};
    std::size_t blockedCount_{};
    std::size_t reportedCount_{};
};

class DirectInput8AProxy final : public IDirectInput8A {
public:
    DirectInput8AProxy(IDirectInput8A* real, HMODULE module) noexcept
        : real_(real), filter_(module) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** output) override {
        if (output == nullptr) return E_POINTER;
        *output = nullptr;
        if (SameGuid(riid, kIidIUnknown) || SameGuid(riid, kIidDirectInput8A)) {
            *output = static_cast<IDirectInput8A*>(this);
            AddRef();
            return S_OK;
        }
        return real_->QueryInterface(riid, output);
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG remaining = InterlockedDecrement(&references_);
        if (remaining == 0) {
            real_->Release();
            delete this;
            return 0;
        }
        return static_cast<ULONG>(remaining);
    }

    bool ReadDevicePath(REFGUID guid, wchar_t* output, std::size_t capacity) noexcept {
        if (output == nullptr || capacity == 0) return false;
        output[0] = L'\0';
        // 直接使用原始 DirectInput 接口，不经过 JoyShim 的设备创建过滤。
        // 这里的 DIPROP_GUIDANDPATH 返回 HID 设备实例路径，匹配失败即放行。
        LPDIRECTINPUTDEVICE8A device = nullptr;
        if (FAILED(real_->CreateDevice(guid, &device, nullptr)) || device == nullptr) return false;
        DIPROPGUIDANDPATH property{};
        property.diph.dwSize = sizeof(property);
        property.diph.dwHeaderSize = sizeof(property.diph);
        property.diph.dwHow = DIPH_DEVICE;
        property.diph.dwObj = 0;
        const HRESULT result = device->GetProperty(DIPROP_GUIDANDPATH, &property.diph);
        device->Release();
        if (FAILED(result)) return false;
        const std::size_t length = BoundedWideLength(
            property.wszPath, std::size(property.wszPath));
        if (length == 0 || length >= capacity) return false;
        std::wmemcpy(output, property.wszPath, length + 1);
        return true;
    }

    HRESULT STDMETHODCALLTYPE CreateDevice(
        REFGUID guid,
        LPDIRECTINPUTDEVICE8A* device,
        LPUNKNOWN outer) override {
        filter_.Refresh();
        if (filter_.IsBlockedInstance(guid)) {
            if (device != nullptr) *device = nullptr;
            return DIERR_DEVICENOTREG;
        }
        return real_->CreateDevice(guid, device, outer);
    }

    HRESULT STDMETHODCALLTYPE EnumDevices(
        DWORD type,
        LPDIENUMDEVICESCALLBACKA callback,
        LPVOID reference,
        DWORD flags) override {
        if (callback == nullptr) return DIERR_INVALIDPARAM;
        filter_.Refresh();
        EnumContext context{this, callback, reference};
        return real_->EnumDevices(type, &EnumThunk, &context, flags);
    }

    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID guid) override {
        if (filter_.IsBlockedInstance(guid)) return DIERR_DEVICENOTREG;
        return real_->GetDeviceStatus(guid);
    }

    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND owner, DWORD flags) override {
        return real_->RunControlPanel(owner, flags);
    }

    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE instance, DWORD version) override {
        return real_->Initialize(instance, version);
    }

    HRESULT STDMETHODCALLTYPE FindDevice(
        REFGUID classGuid,
        LPCSTR name,
        LPGUID instanceGuid) override {
        const HRESULT result = real_->FindDevice(classGuid, name, instanceGuid);
        if (SUCCEEDED(result) && instanceGuid != nullptr &&
            filter_.IsBlockedInstance(*instanceGuid)) {
            return DIERR_DEVICENOTREG;
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(
        LPCSTR userName,
        LPDIACTIONFORMATA actionFormat,
        LPDIENUMDEVICESBYSEMANTICSCBA callback,
        LPVOID reference,
        DWORD flags) override {
        if (callback == nullptr) return DIERR_INVALIDPARAM;
        filter_.Refresh();
        SemanticsContext context{this, callback, reference};
        return real_->EnumDevicesBySemantics(
            userName,
            actionFormat,
            &SemanticsThunk,
            &context,
            flags);
    }

    HRESULT STDMETHODCALLTYPE ConfigureDevices(
        LPDICONFIGUREDEVICESCALLBACK callback,
        LPDICONFIGUREDEVICESPARAMSA parameters,
        DWORD flags,
        LPVOID reference) override {
        return real_->ConfigureDevices(callback, parameters, flags, reference);
    }

private:
    struct EnumContext {
        DirectInput8AProxy* self;
        LPDIENUMDEVICESCALLBACKA callback;
        LPVOID reference;
    };

    struct SemanticsContext {
        DirectInput8AProxy* self;
        LPDIENUMDEVICESBYSEMANTICSCBA callback;
        LPVOID reference;
    };

    static BOOL CALLBACK EnumThunk(
        LPCDIDEVICEINSTANCEA instance,
        LPVOID reference) noexcept {
        auto* context = static_cast<EnumContext*>(reference);
        if (context == nullptr || instance == nullptr) return DIENUM_STOP;
        const DirectInputDeviceIdentity identity =
            MakeIdentity(instance->guidProduct, instance->tszProductName);
        wchar_t instancePath[MaxManagedDevicePath]{};
        if (IsDirectInputGameController(
                instance->dwDevType, instance->wUsagePage, instance->wUsage)) {
            context->self->ReadDevicePath(
                instance->guidInstance, instancePath, std::size(instancePath));
        }
        if (context->self->filter_.ShouldBlock(
                instance->guidInstance, identity,
                instance->dwDevType, instance->wUsagePage, instance->wUsage,
                instancePath)) {
            return DIENUM_CONTINUE;
        }
        return context->callback(instance, context->reference);
    }

    static BOOL CALLBACK SemanticsThunk(
        LPCDIDEVICEINSTANCEA instance,
        LPDIRECTINPUTDEVICE8A device,
        DWORD flags,
        DWORD remaining,
        LPVOID reference) noexcept {
        auto* context = static_cast<SemanticsContext*>(reference);
        if (context == nullptr || instance == nullptr) return DIENUM_STOP;
        const DirectInputDeviceIdentity identity =
            MakeIdentity(instance->guidProduct, instance->tszProductName);
        wchar_t instancePath[MaxManagedDevicePath]{};
        if (IsDirectInputGameController(
                instance->dwDevType, instance->wUsagePage, instance->wUsage)) {
            context->self->ReadDevicePath(
                instance->guidInstance, instancePath, std::size(instancePath));
        }
        if (context->self->filter_.ShouldBlock(
                instance->guidInstance, identity,
                instance->dwDevType, instance->wUsagePage, instance->wUsage,
                instancePath)) {
            return DIENUM_CONTINUE;
        }
        return context->callback(
            instance, device, flags, remaining, context->reference);
    }

    volatile LONG references_{1};
    IDirectInput8A* real_{};
    FilterState filter_;
};

class DirectInput8WProxy final : public IDirectInput8W {
public:
    DirectInput8WProxy(IDirectInput8W* real, HMODULE module) noexcept
        : real_(real), filter_(module) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** output) override {
        if (output == nullptr) return E_POINTER;
        *output = nullptr;
        if (SameGuid(riid, kIidIUnknown) || SameGuid(riid, kIidDirectInput8W)) {
            *output = static_cast<IDirectInput8W*>(this);
            AddRef();
            return S_OK;
        }
        return real_->QueryInterface(riid, output);
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG remaining = InterlockedDecrement(&references_);
        if (remaining == 0) {
            real_->Release();
            delete this;
            return 0;
        }
        return static_cast<ULONG>(remaining);
    }

    bool ReadDevicePath(REFGUID guid, wchar_t* output, std::size_t capacity) noexcept {
        if (output == nullptr || capacity == 0) return false;
        output[0] = L'\0';
        // 直接使用原始 DirectInput 接口，不经过 JoyShim 的设备创建过滤。
        // 这里的 DIPROP_GUIDANDPATH 返回 HID 设备实例路径，匹配失败即放行。
        LPDIRECTINPUTDEVICE8W device = nullptr;
        if (FAILED(real_->CreateDevice(guid, &device, nullptr)) || device == nullptr) return false;
        DIPROPGUIDANDPATH property{};
        property.diph.dwSize = sizeof(property);
        property.diph.dwHeaderSize = sizeof(property.diph);
        property.diph.dwHow = DIPH_DEVICE;
        property.diph.dwObj = 0;
        const HRESULT result = device->GetProperty(DIPROP_GUIDANDPATH, &property.diph);
        device->Release();
        if (FAILED(result)) return false;
        const std::size_t length = BoundedWideLength(
            property.wszPath, std::size(property.wszPath));
        if (length == 0 || length >= capacity) return false;
        std::wmemcpy(output, property.wszPath, length + 1);
        return true;
    }

    HRESULT STDMETHODCALLTYPE CreateDevice(
        REFGUID guid,
        LPDIRECTINPUTDEVICE8W* device,
        LPUNKNOWN outer) override {
        filter_.Refresh();
        if (filter_.IsBlockedInstance(guid)) {
            if (device != nullptr) *device = nullptr;
            return DIERR_DEVICENOTREG;
        }
        return real_->CreateDevice(guid, device, outer);
    }

    HRESULT STDMETHODCALLTYPE EnumDevices(
        DWORD type,
        LPDIENUMDEVICESCALLBACKW callback,
        LPVOID reference,
        DWORD flags) override {
        if (callback == nullptr) return DIERR_INVALIDPARAM;
        filter_.Refresh();
        EnumContext context{this, callback, reference};
        return real_->EnumDevices(type, &EnumThunk, &context, flags);
    }

    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID guid) override {
        if (filter_.IsBlockedInstance(guid)) return DIERR_DEVICENOTREG;
        return real_->GetDeviceStatus(guid);
    }

    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND owner, DWORD flags) override {
        return real_->RunControlPanel(owner, flags);
    }

    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE instance, DWORD version) override {
        return real_->Initialize(instance, version);
    }

    HRESULT STDMETHODCALLTYPE FindDevice(
        REFGUID classGuid,
        LPCWSTR name,
        LPGUID instanceGuid) override {
        const HRESULT result = real_->FindDevice(classGuid, name, instanceGuid);
        if (SUCCEEDED(result) && instanceGuid != nullptr &&
            filter_.IsBlockedInstance(*instanceGuid)) {
            return DIERR_DEVICENOTREG;
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(
        LPCWSTR userName,
        LPDIACTIONFORMATW actionFormat,
        LPDIENUMDEVICESBYSEMANTICSCBW callback,
        LPVOID reference,
        DWORD flags) override {
        if (callback == nullptr) return DIERR_INVALIDPARAM;
        filter_.Refresh();
        SemanticsContext context{this, callback, reference};
        return real_->EnumDevicesBySemantics(
            userName,
            actionFormat,
            &SemanticsThunk,
            &context,
            flags);
    }

    HRESULT STDMETHODCALLTYPE ConfigureDevices(
        LPDICONFIGUREDEVICESCALLBACK callback,
        LPDICONFIGUREDEVICESPARAMSW parameters,
        DWORD flags,
        LPVOID reference) override {
        return real_->ConfigureDevices(callback, parameters, flags, reference);
    }

private:
    struct EnumContext {
        DirectInput8WProxy* self;
        LPDIENUMDEVICESCALLBACKW callback;
        LPVOID reference;
    };

    struct SemanticsContext {
        DirectInput8WProxy* self;
        LPDIENUMDEVICESBYSEMANTICSCBW callback;
        LPVOID reference;
    };

    static BOOL CALLBACK EnumThunk(
        LPCDIDEVICEINSTANCEW instance,
        LPVOID reference) noexcept {
        auto* context = static_cast<EnumContext*>(reference);
        if (context == nullptr || instance == nullptr) return DIENUM_STOP;
        const DirectInputDeviceIdentity identity =
            MakeIdentity(instance->guidProduct, instance->tszProductName);
        wchar_t instancePath[MaxManagedDevicePath]{};
        if (IsDirectInputGameController(
                instance->dwDevType, instance->wUsagePage, instance->wUsage)) {
            context->self->ReadDevicePath(
                instance->guidInstance, instancePath, std::size(instancePath));
        }
        if (context->self->filter_.ShouldBlock(
                instance->guidInstance, identity,
                instance->dwDevType, instance->wUsagePage, instance->wUsage,
                instancePath)) {
            return DIENUM_CONTINUE;
        }
        return context->callback(instance, context->reference);
    }

    static BOOL CALLBACK SemanticsThunk(
        LPCDIDEVICEINSTANCEW instance,
        LPDIRECTINPUTDEVICE8W device,
        DWORD flags,
        DWORD remaining,
        LPVOID reference) noexcept {
        auto* context = static_cast<SemanticsContext*>(reference);
        if (context == nullptr || instance == nullptr) return DIENUM_STOP;
        const DirectInputDeviceIdentity identity =
            MakeIdentity(instance->guidProduct, instance->tszProductName);
        wchar_t instancePath[MaxManagedDevicePath]{};
        if (IsDirectInputGameController(
                instance->dwDevType, instance->wUsagePage, instance->wUsage)) {
            context->self->ReadDevicePath(
                instance->guidInstance, instancePath, std::size(instancePath));
        }
        if (context->self->filter_.ShouldBlock(
                instance->guidInstance, identity,
                instance->dwDevType, instance->wUsagePage, instance->wUsage,
                instancePath)) {
            return DIENUM_CONTINUE;
        }
        return context->callback(
            instance, device, flags, remaining, context->reference);
    }

    volatile LONG references_{1};
    IDirectInput8W* real_{};
    FilterState filter_;
};

}  // namespace

using DirectInput8CreateFunction =
    HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

DirectInput8CreateFunction RealDirectInput8Create() noexcept {
    static const auto function = FarProcToFunction<DirectInput8CreateFunction>(
        ResolveSystemExport(SystemDllKind::DInput8, "DirectInput8Create"));
    return function;
}

HRESULT WINAPI ConfiguredDirectInput8Create(
    HINSTANCE instance,
    DWORD version,
    const GUID& interfaceId,
    void** output,
    void* outer) noexcept {
    DirectInput8CreateFunction function = RealDirectInput8Create();
    if (function == nullptr) return E_FAIL;
    if (output == nullptr) return E_POINTER;

    const RuntimeConfig config = LoadRuntimeConfig(ProxyEntryModule());
    if (!ConfigRequestsDirectInputInterception(config)) {
        return function(
            instance,
            version,
            interfaceId,
            output,
            static_cast<LPUNKNOWN>(outer));
    }
    return FilteredDirectInput8Create(
        instance, version, interfaceId, output, outer);
}

HRESULT WINAPI FilteredDirectInput8Create(
    HINSTANCE instance,
    DWORD version,
    const GUID& interfaceId,
    void** output,
    void* outer) noexcept {
    DirectInput8CreateFunction function = RealDirectInput8Create();
    if (function == nullptr) return E_FAIL;
    if (output == nullptr) return E_POINTER;
    *output = nullptr;

    ProxyEntryNotifyDirectInputCreate();
    static volatile LONG reported = 0;
    if (InterlockedCompareExchange(&reported, 1, 0) == 0) {
        const RuntimeConfig config = LoadRuntimeConfig(ProxyEntryModule());
        wchar_t message[384]{};
        std::swprintf(
            message,
            std::size(message),
            L"DirectInput8Create 已进入 JoyShim 包装层：入口=%ls。",
            ProxyEntryCurrentName());
        WriteLogLine(config, message);
    }

    void* real = nullptr;
    const HRESULT result = function(
        instance, version, interfaceId, &real, static_cast<LPUNKNOWN>(outer));
    if (FAILED(result) || real == nullptr) return result;

    HMODULE module = ProxyEntryModule();
    if (SameGuid(interfaceId, kIidDirectInput8A)) {
        auto* proxy = new (std::nothrow) DirectInput8AProxy(
            static_cast<IDirectInput8A*>(real), module);
        if (proxy == nullptr) {
            static_cast<IDirectInput8A*>(real)->Release();
            return E_OUTOFMEMORY;
        }
        *output = static_cast<IDirectInput8A*>(proxy);
        return result;
    }
    if (SameGuid(interfaceId, kIidDirectInput8W)) {
        auto* proxy = new (std::nothrow) DirectInput8WProxy(
            static_cast<IDirectInput8W*>(real), module);
        if (proxy == nullptr) {
            static_cast<IDirectInput8W*>(real)->Release();
            return E_OUTOFMEMORY;
        }
        *output = static_cast<IDirectInput8W*>(proxy);
        return result;
    }

    // 未知接口保持透明，不冒险包装非 IDirectInput8A/W 对象。
    *output = real;
    return result;
}

}  // namespace joyshim
