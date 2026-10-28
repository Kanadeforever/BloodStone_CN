#include "proxy_entry.hpp"

#include <tlhelp32.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iterator>

#include "config.hpp"
#include "directinput_filter.hpp"
#include "runtime_start.hpp"
#include "win32_proc.hpp"
#include "xinput_exports.hpp"

#ifndef IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT
#define IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT 13
#endif

namespace joyshim {

extern "C" FARPROC WINAPI JoyShimGetProcAddress(
    HMODULE module,
    LPCSTR nameOrOrdinal) noexcept;

namespace {

constexpr LONG HookXInput = 1 << 0;
constexpr LONG HookDirectInput = 1 << 1;
constexpr LONG HookDynamicLookup = 1 << 2;
constexpr ULONGLONG ModuleRefreshIntervalMs = 1000;
constexpr DWORD DeferredBootstrapDelayMs = 500;
constexpr DWORD DelayDescriptorUsesRva = 1;

// PE 延迟导入描述符。字段布局与 delayimp.h 的 ImgDelayDescr 相同；在此
// 本地定义，避免把实现绑定到某个工具链私有头文件。
struct DelayImportDescriptor {
    DWORD attributes;
    DWORD name;
    DWORD moduleHandle;
    DWORD importAddressTable;
    DWORD importNameTable;
    DWORD boundImportAddressTable;
    DWORD unloadImportAddressTable;
    DWORD timestamp;
};

struct SystemModuleState {
    INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    HMODULE module{};
    const wchar_t* name{};
};

SystemModuleState g_xinput11{INIT_ONCE_STATIC_INIT, nullptr, L"xinput1_1.dll"};
SystemModuleState g_xinput12{INIT_ONCE_STATIC_INIT, nullptr, L"xinput1_2.dll"};
SystemModuleState g_xinput13{INIT_ONCE_STATIC_INIT, nullptr, L"xinput1_3.dll"};
SystemModuleState g_xinput14{INIT_ONCE_STATIC_INIT, nullptr, L"xinput1_4.dll"};
SystemModuleState g_xinput910{INIT_ONCE_STATIC_INIT, nullptr, L"xinput9_1_0.dll"};
SystemModuleState g_dinput8{INIT_ONCE_STATIC_INIT, nullptr, L"dinput8.dll"};

HMODULE g_selfModule{};
ProxyEntryKind g_entryKind{ProxyEntryKind::Unknown};
wchar_t g_entryName[32]{L"(未知)"};
wchar_t g_gameRoot[MAX_PATH]{};
volatile LONG g_hookMode{HookXInput | HookDynamicLookup};
volatile LONG g_bootstrapQueued{};
volatile LONG g_detaching{};
volatile LONG64 g_nextRefreshTick{};
SRWLOCK g_patchLock = SRWLOCK_INIT;

volatile LONG g_modulesVisited{};
volatile LONG g_normalDescriptors{};
volatile LONG g_delayDescriptors{};
volatile LONG g_xinputPatched{};
volatile LONG g_directInputPatched{};
volatile LONG g_getProcAddressPatched{};
volatile LONG g_patchFailures{};
volatile LONG g_dynamicXInputRedirects{};
volatile LONG g_dynamicDirectInputRedirects{};
volatile LONG g_directInputCreateCalls{};
volatile LONG g_directInputDevicesHidden{};
volatile LONG g_earlyXInputPatched{};
volatile LONG g_earlyDirectInputPatched{};
volatile LONG g_earlyGetProcAddressPatched{};
volatile LONG g_earlyPatchFailures{};

bool EqualsIgnoreCase(const wchar_t* left, const wchar_t* right) noexcept {
    if (left == nullptr || right == nullptr) return false;
    while (*left != L'\0' && *right != L'\0') {
        if (std::towlower(*left) != std::towlower(*right)) return false;
        ++left;
        ++right;
    }
    return *left == *right;
}

bool StartsWithIgnoreCase(const wchar_t* text, const wchar_t* prefix) noexcept {
    if (text == nullptr || prefix == nullptr) return false;
    while (*prefix != L'\0') {
        if (*text == L'\0' || std::towlower(*text) != std::towlower(*prefix)) {
            return false;
        }
        ++text;
        ++prefix;
    }
    return true;
}

const wchar_t* BaseName(const wchar_t* path) noexcept {
    if (path == nullptr) return L"";
    const wchar_t* slash = std::wcsrchr(path, L'\\');
    return slash != nullptr ? slash + 1 : path;
}

void CopyWide(const wchar_t* source, wchar_t* target, std::size_t capacity) noexcept {
    if (target == nullptr || capacity == 0) return;
    target[0] = L'\0';
    if (source == nullptr) return;
    const std::size_t length = std::wcslen(source);
    const std::size_t count = length < capacity - 1 ? length : capacity - 1;
    if (count != 0) std::wmemcpy(target, source, count);
    target[count] = L'\0';
}

bool IsXInputName(const wchar_t* name) noexcept {
    return EqualsIgnoreCase(name, L"xinput1_1.dll") ||
        EqualsIgnoreCase(name, L"xinput1_2.dll") ||
        EqualsIgnoreCase(name, L"xinput1_3.dll") ||
        EqualsIgnoreCase(name, L"xinput1_4.dll") ||
        EqualsIgnoreCase(name, L"xinput9_1_0.dll");
}

bool IsXInputName(const char* name) noexcept {
    return name != nullptr &&
        (_stricmp(name, "xinput1_1.dll") == 0 ||
         _stricmp(name, "xinput1_2.dll") == 0 ||
         _stricmp(name, "xinput1_3.dll") == 0 ||
         _stricmp(name, "xinput1_4.dll") == 0 ||
         _stricmp(name, "xinput9_1_0.dll") == 0);
}

ProxyEntryKind ClassifyEntry(const wchar_t* name) noexcept {
    if (EqualsIgnoreCase(name, L"xinput1_1.dll")) return ProxyEntryKind::XInput11;
    if (EqualsIgnoreCase(name, L"xinput1_2.dll")) return ProxyEntryKind::XInput12;
    if (EqualsIgnoreCase(name, L"xinput1_3.dll")) return ProxyEntryKind::XInput13;
    if (EqualsIgnoreCase(name, L"xinput1_4.dll")) return ProxyEntryKind::XInput14;
    if (EqualsIgnoreCase(name, L"xinput9_1_0.dll")) return ProxyEntryKind::XInput910;
    if (EqualsIgnoreCase(name, L"dinput8.dll")) return ProxyEntryKind::DInput8;
    if (EqualsIgnoreCase(name, L"winmm.dll")) return ProxyEntryKind::WinMM;
    return ProxyEntryKind::Unknown;
}

bool IsKnownProxyInfrastructure(const wchar_t* name) noexcept {
    return EqualsIgnoreCase(name, L"SDL3.dll") || IsXInputName(name) ||
        EqualsIgnoreCase(name, L"dinput8.dll") ||
        EqualsIgnoreCase(name, L"winmm.dll") ||
        EqualsIgnoreCase(name, L"JoyShim.dll");
}

bool RangeInsideImage(
    std::size_t imageSize,
    std::uint32_t rva,
    std::size_t bytes) noexcept {
    const std::size_t offset = static_cast<std::size_t>(rva);
    return offset <= imageSize && bytes <= imageSize - offset;
}

bool AddressInsideImage(
    const std::uint8_t* base,
    std::size_t imageSize,
    const void* address,
    std::size_t bytes) noexcept {
    if (base == nullptr || address == nullptr) return false;
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(base);
    const std::uintptr_t end = begin + imageSize;
    const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(address);
    return value >= begin && value <= end && bytes <= end - value;
}

bool NullTerminatedAnsiInsideImage(
    const std::uint8_t* base,
    std::size_t imageSize,
    std::uint32_t rva) noexcept {
    if (base == nullptr || !RangeInsideImage(imageSize, rva, 1)) return false;
    const char* text = reinterpret_cast<const char*>(base + rva);
    const std::size_t maximum = imageSize - static_cast<std::size_t>(rva);
    return std::memchr(text, '\0', maximum) != nullptr;
}

enum class PatchPointerResult {
    Unchanged,
    Patched,
    Failed,
};

PatchPointerResult PatchPointer(ULONG_PTR* slot, ULONG_PTR replacement) noexcept {
    if (slot == nullptr || replacement == 0) return PatchPointerResult::Failed;
    if (*slot == replacement) return PatchPointerResult::Unchanged;
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtect)) {
        return PatchPointerResult::Failed;
    }
    InterlockedExchangePointer(
        reinterpret_cast<void* volatile*>(slot),
        reinterpret_cast<void*>(replacement));
    DWORD ignored = 0;
    const BOOL restored = VirtualProtect(slot, sizeof(*slot), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
    return restored ? PatchPointerResult::Patched : PatchPointerResult::Failed;
}

BOOL CALLBACK LoadSystemModule(PINIT_ONCE, PVOID parameter, PVOID*) noexcept {
    auto* state = static_cast<SystemModuleState*>(parameter);
    if (state == nullptr || state->name == nullptr) return TRUE;
    wchar_t path[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return TRUE;
    const std::size_t baseLength = std::wcslen(path);
    const std::size_t nameLength = std::wcslen(state->name);
    if (baseLength + 1 + nameLength + 1 > MAX_PATH) return TRUE;
    path[baseLength] = L'\\';
    std::wmemcpy(path + baseLength + 1, state->name, nameLength + 1);
    state->module = LoadLibraryW(path);
    return TRUE;
}

SystemModuleState* SystemStateFor(const char* name) noexcept {
    if (name == nullptr) return nullptr;
    if (_stricmp(name, "xinput1_1.dll") == 0) return &g_xinput11;
    if (_stricmp(name, "xinput1_2.dll") == 0) return &g_xinput12;
    if (_stricmp(name, "xinput1_3.dll") == 0) return &g_xinput13;
    if (_stricmp(name, "xinput1_4.dll") == 0) return &g_xinput14;
    if (_stricmp(name, "xinput9_1_0.dll") == 0) return &g_xinput910;
    if (_stricmp(name, "dinput8.dll") == 0) return &g_dinput8;
    return nullptr;
}

HMODULE LoadSystemModuleFor(const char* name) noexcept {
    SystemModuleState* state = SystemStateFor(name);
    if (state == nullptr) return nullptr;
    InitOnceExecuteOnce(&state->once, LoadSystemModule, state, nullptr);
    return state->module;
}

FARPROC ReplacementByName(
    const char* importedDll,
    LPCSTR nameOrOrdinal,
    LONG mode) noexcept {
    if ((mode & HookXInput) != 0 && IsXInputName(importedDll)) {
        return ResolveLocalXInputExport(nameOrOrdinal);
    }
    if ((mode & HookDirectInput) != 0 && importedDll != nullptr &&
        _stricmp(importedDll, "dinput8.dll") == 0 &&
        nameOrOrdinal != nullptr && !IS_INTRESOURCE(nameOrOrdinal) &&
        std::strcmp(nameOrOrdinal, "DirectInput8Create") == 0) {
        return FunctionToFarProc(&ConfiguredDirectInput8Create);
    }
    if ((mode & HookDynamicLookup) != 0 && nameOrOrdinal != nullptr &&
        !IS_INTRESOURCE(nameOrOrdinal) &&
        std::strcmp(nameOrOrdinal, "GetProcAddress") == 0) {
        return FunctionToFarProc(&JoyShimGetProcAddress);
    }
    return nullptr;
}

FARPROC ReplacementByResolvedAddress(
    const char* importedDll,
    FARPROC current,
    LONG mode) noexcept {
    if (current == nullptr) return nullptr;

    if ((mode & HookXInput) != 0 && IsXInputName(importedDll)) {
        HMODULE system = LoadSystemModuleFor(importedDll);
        if (system != nullptr) {
            constexpr std::array<const char*, 10> names = {
                "XInputGetState",
                "XInputSetState",
                "XInputGetCapabilities",
                "XInputEnable",
                "XInputGetDSoundAudioDeviceGuids",
                "XInputGetBatteryInformation",
                "XInputGetKeystroke",
                "XInputGetAudioDeviceIds",
                "XInputGetStateEx",
                "XInputGetCapabilitiesEx",
            };
            for (const char* name : names) {
                const FARPROC systemExport = GetProcAddress(system, name);
                if (systemExport == current) return ResolveLocalXInputExport(name);
            }
            constexpr std::array<WORD, 12> ordinals = {
                2, 3, 4, 5, 6, 7, 8, 100, 101, 102, 103, 108,
            };
            for (WORD ordinal : ordinals) {
                const LPCSTR resource = MAKEINTRESOURCEA(ordinal);
                const FARPROC systemExport = GetProcAddress(system, resource);
                if (systemExport == current) return ResolveLocalXInputExport(resource);
            }
        }
    }

    if ((mode & HookDirectInput) != 0 && importedDll != nullptr &&
        _stricmp(importedDll, "dinput8.dll") == 0) {
        HMODULE system = LoadSystemModuleFor(importedDll);
        if (system != nullptr &&
            GetProcAddress(system, "DirectInput8Create") == current) {
            return FunctionToFarProc(&ConfiguredDirectInput8Create);
        }
    }

    if ((mode & HookDynamicLookup) != 0) {
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
        const FARPROC kernel32Address = kernel32 != nullptr
            ? GetProcAddress(kernel32, "GetProcAddress") : nullptr;
        const FARPROC kernelBaseAddress = kernelBase != nullptr
            ? GetProcAddress(kernelBase, "GetProcAddress") : nullptr;
        if (current == kernel32Address || current == kernelBaseAddress ||
            current == FunctionToFarProc(&GetProcAddress)) {
            return FunctionToFarProc(&JoyShimGetProcAddress);
        }
    }
    return nullptr;
}

void RecordPatchResult(PatchPointerResult result, volatile LONG* counter) noexcept {
    if (result == PatchPointerResult::Patched) {
        InterlockedIncrement(counter);
    } else if (result == PatchPointerResult::Failed) {
        InterlockedIncrement(&g_patchFailures);
    }
}

void PatchThunkArrays(
    std::uint8_t* base,
    std::size_t imageSize,
    const char* importedDll,
    IMAGE_THUNK_DATA* nameTable,
    IMAGE_THUNK_DATA* addressTable,
    std::size_t maximum,
    LONG mode,
    bool allowResolvedAddress) noexcept {
    if (base == nullptr || addressTable == nullptr) return;
    for (std::size_t index = 0; index < maximum; ++index) {
        IMAGE_THUNK_DATA* address = &addressTable[index];
        if (address->u1.Function == 0) break;

        FARPROC replacement = nullptr;
        if (nameTable != nullptr) {
            const IMAGE_THUNK_DATA& source = nameTable[index];
            if (source.u1.AddressOfData == 0) break;
            LPCSTR importName = nullptr;
            if (IMAGE_SNAP_BY_ORDINAL(source.u1.Ordinal)) {
                importName = MAKEINTRESOURCEA(IMAGE_ORDINAL(source.u1.Ordinal));
            } else {
                const std::uint32_t nameRva =
                    static_cast<std::uint32_t>(source.u1.AddressOfData);
                if (!RangeInsideImage(imageSize, nameRva, sizeof(IMAGE_IMPORT_BY_NAME)) ||
                    !NullTerminatedAnsiInsideImage(
                        base,
                        imageSize,
                        nameRva + static_cast<std::uint32_t>(
                            offsetof(IMAGE_IMPORT_BY_NAME, Name)))) {
                    continue;
                }
                const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                    base + nameRva);
                importName = reinterpret_cast<LPCSTR>(byName->Name);
            }
            replacement = ReplacementByName(importedDll, importName, mode);
        } else if (allowResolvedAddress) {
            replacement = ReplacementByResolvedAddress(
                importedDll,
                reinterpret_cast<FARPROC>(address->u1.Function),
                mode);
        }
        if (replacement == nullptr) continue;

        volatile LONG* counter = &g_getProcAddressPatched;
        if (ResolveLocalXInputExport(
                nameTable != nullptr && IMAGE_SNAP_BY_ORDINAL(nameTable[index].u1.Ordinal)
                    ? MAKEINTRESOURCEA(IMAGE_ORDINAL(nameTable[index].u1.Ordinal))
                    : nullptr) == replacement ||
            (importedDll != nullptr && IsXInputName(importedDll))) {
            counter = &g_xinputPatched;
        } else if (replacement == FunctionToFarProc(&ConfiguredDirectInput8Create)) {
            counter = &g_directInputPatched;
        }
        RecordPatchResult(
            PatchPointer(
                &address->u1.Function,
                reinterpret_cast<ULONG_PTR>(replacement)),
            counter);
    }
}

void PatchNormalImports(
    std::uint8_t* base,
    std::size_t imageSize,
    const IMAGE_DATA_DIRECTORY& directory,
    LONG mode,
    bool allowResolvedAddress) noexcept {
    if (directory.VirtualAddress == 0 ||
        directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR) ||
        !RangeInsideImage(imageSize, directory.VirtualAddress, directory.Size)) {
        return;
    }
    auto* descriptors = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + directory.VirtualAddress);
    const std::size_t maximumDescriptors =
        directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    for (std::size_t descriptorIndex = 0;
         descriptorIndex < maximumDescriptors;
         ++descriptorIndex) {
        IMAGE_IMPORT_DESCRIPTOR& descriptor = descriptors[descriptorIndex];
        if (descriptor.Name == 0) break;
        if (!NullTerminatedAnsiInsideImage(base, imageSize, descriptor.Name) ||
            descriptor.FirstThunk == 0 ||
            !RangeInsideImage(
                imageSize,
                descriptor.FirstThunk,
                sizeof(IMAGE_THUNK_DATA))) {
            continue;
        }
        InterlockedIncrement(&g_normalDescriptors);
        const char* importedDll = reinterpret_cast<const char*>(base + descriptor.Name);
        auto* addressTable = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + descriptor.FirstThunk);
        const std::size_t addressMaximum =
            (imageSize - static_cast<std::size_t>(descriptor.FirstThunk)) /
            sizeof(IMAGE_THUNK_DATA);

        IMAGE_THUNK_DATA* nameTable = nullptr;
        std::size_t maximum = addressMaximum;
        if (descriptor.OriginalFirstThunk != 0 &&
            RangeInsideImage(
                imageSize,
                descriptor.OriginalFirstThunk,
                sizeof(IMAGE_THUNK_DATA))) {
            nameTable = reinterpret_cast<IMAGE_THUNK_DATA*>(
                base + descriptor.OriginalFirstThunk);
            const std::size_t nameMaximum =
                (imageSize - static_cast<std::size_t>(descriptor.OriginalFirstThunk)) /
                sizeof(IMAGE_THUNK_DATA);
            if (nameMaximum < maximum) maximum = nameMaximum;
        }
        PatchThunkArrays(
            base,
            imageSize,
            importedDll,
            nameTable,
            addressTable,
            maximum,
            mode,
            allowResolvedAddress);
    }
}

bool NullTerminatedAnsiPointerInsideImage(
    const std::uint8_t* base,
    std::size_t imageSize,
    const char* text) noexcept {
    if (!AddressInsideImage(base, imageSize, text, 1)) return false;
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(base);
    const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(text);
    const std::size_t maximum = imageSize - static_cast<std::size_t>(value - begin);
    return std::memchr(text, '\0', maximum) != nullptr;
}

void* DelayFieldPointer(
    std::uint8_t* base,
    std::size_t imageSize,
    DWORD field,
    DWORD attributes,
    std::size_t minimumBytes) noexcept {
    if (field == 0) return nullptr;
    if ((attributes & DelayDescriptorUsesRva) != 0) {
        return RangeInsideImage(imageSize, field, minimumBytes)
            ? static_cast<void*>(base + field) : nullptr;
    }
    void* address = reinterpret_cast<void*>(static_cast<std::uintptr_t>(field));
    return AddressInsideImage(base, imageSize, address, minimumBytes)
        ? address : nullptr;
}

void PatchDelayImports(
    std::uint8_t* base,
    std::size_t imageSize,
    const IMAGE_DATA_DIRECTORY& directory,
    LONG mode) noexcept {
    if (directory.VirtualAddress == 0 ||
        directory.Size < sizeof(DelayImportDescriptor) ||
        !RangeInsideImage(imageSize, directory.VirtualAddress, directory.Size)) {
        return;
    }
    auto* descriptors = reinterpret_cast<DelayImportDescriptor*>(
        base + directory.VirtualAddress);
    const std::size_t maximumDescriptors =
        directory.Size / sizeof(DelayImportDescriptor);
    for (std::size_t descriptorIndex = 0;
         descriptorIndex < maximumDescriptors;
         ++descriptorIndex) {
        DelayImportDescriptor& descriptor = descriptors[descriptorIndex];
        if (descriptor.name == 0) break;
        auto* importedDll = static_cast<const char*>(DelayFieldPointer(
            base, imageSize, descriptor.name, descriptor.attributes, 1));
        auto* addressTable = static_cast<IMAGE_THUNK_DATA*>(DelayFieldPointer(
            base,
            imageSize,
            descriptor.importAddressTable,
            descriptor.attributes,
            sizeof(IMAGE_THUNK_DATA)));
        auto* nameTable = static_cast<IMAGE_THUNK_DATA*>(DelayFieldPointer(
            base,
            imageSize,
            descriptor.importNameTable,
            descriptor.attributes,
            sizeof(IMAGE_THUNK_DATA)));
        if (importedDll == nullptr || addressTable == nullptr || nameTable == nullptr ||
            !NullTerminatedAnsiPointerInsideImage(base, imageSize, importedDll)) {
            continue;
        }
        const std::uintptr_t baseAddress = reinterpret_cast<std::uintptr_t>(base);
        const std::uintptr_t addressValue = reinterpret_cast<std::uintptr_t>(addressTable);
        const std::uintptr_t nameValue = reinterpret_cast<std::uintptr_t>(nameTable);
        const std::size_t addressMaximum =
            (imageSize - static_cast<std::size_t>(addressValue - baseAddress)) /
            sizeof(IMAGE_THUNK_DATA);
        const std::size_t nameMaximum =
            (imageSize - static_cast<std::size_t>(nameValue - baseAddress)) /
            sizeof(IMAGE_THUNK_DATA);
        const std::size_t maximum = addressMaximum < nameMaximum
            ? addressMaximum : nameMaximum;
        InterlockedIncrement(&g_delayDescriptors);
        PatchThunkArrays(
            base,
            imageSize,
            importedDll,
            nameTable,
            addressTable,
            maximum,
            mode,
            true);
    }
}

bool ReadImage(
    HMODULE module,
    std::uint8_t*& base,
    std::size_t& imageSize,
    const IMAGE_NT_HEADERS*& nt) noexcept {
    base = reinterpret_cast<std::uint8_t*>(module);
    imageSize = 0;
    nt = nullptr;
    if (base == nullptr) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    const std::size_t ntOffset = static_cast<std::size_t>(dos->e_lfanew);
    nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + ntOffset);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    imageSize = static_cast<std::size_t>(nt->OptionalHeader.SizeOfImage);
    return imageSize >= ntOffset + sizeof(IMAGE_NT_HEADERS);
}

void PatchModule(HMODULE module, LONG mode, bool includeDelayImports, bool allowResolvedAddress) noexcept {
    if (module == nullptr || module == g_selfModule || mode == 0) return;
    std::uint8_t* base = nullptr;
    std::size_t imageSize = 0;
    const IMAGE_NT_HEADERS* nt = nullptr;
    if (!ReadImage(module, base, imageSize, nt)) {
        InterlockedIncrement(&g_patchFailures);
        return;
    }
    InterlockedIncrement(&g_modulesVisited);
    PatchNormalImports(
        base,
        imageSize,
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT],
        mode,
        allowResolvedAddress);
    // 直接遍历延迟导入描述符在部分老游戏的启动阶段风险较高。初始
    // 接管只处理普通 IAT；延迟加载通过已接管的 GetProcAddress 路径覆盖。
    // includeDelayImports 仅保留给显式诊断构建，发行路径默认关闭。
    if (includeDelayImports) {
        PatchDelayImports(
            base,
            imageSize,
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT],
            mode);
    }
}

bool IsGameModulePath(const wchar_t* path, HMODULE module) noexcept {
    if (module == nullptr || module == g_selfModule || path == nullptr || path[0] == L'\0') {
        return false;
    }
    if (module == GetModuleHandleW(nullptr)) return true;
    if (g_gameRoot[0] == L'\0' || !StartsWithIgnoreCase(path, g_gameRoot)) return false;
    const std::size_t rootLength = std::wcslen(g_gameRoot);
    if (path[rootLength] != L'\\' && path[rootLength] != L'\0') return false;
    return !IsKnownProxyInfrastructure(BaseName(path));
}

void PatchCurrentGameModules(bool includeLoadedModules) noexcept {
    const LONG mode = InterlockedCompareExchange(&g_hookMode, 0, 0);
    HMODULE mainModule = GetModuleHandleW(nullptr);
    PatchModule(mainModule, mode, false, true);
    if (!includeLoadedModules) return;

    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
        GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE || snapshot == nullptr) {
        InterlockedIncrement(&g_patchFailures);
        return;
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (entry.hModule != mainModule &&
                IsGameModulePath(entry.szExePath, entry.hModule)) {
                PatchModule(entry.hModule, mode, false, true);
            }
            entry.dwSize = sizeof(entry);
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

void LogHookSnapshot(const RuntimeConfig& config, const wchar_t* stage) noexcept {
    if (!config.enableLog) return;
    const ProxyHookSnapshot snapshot = ProxyEntryHookSnapshot();
    wchar_t message[1024]{};
    std::swprintf(
        message,
        std::size(message),
        L"JoyShim 入口接管：阶段=%ls，入口=%ls，扫描模块=%u，普通导入描述符=%u，延迟导入描述符=%u，XInput槽位=%u，DirectInput槽位=%u，GetProcAddress槽位=%u，动态XInput重定向=%u，动态DirectInput重定向=%u，失败=%u。",
        stage != nullptr ? stage : L"(未知)",
        ProxyEntryCurrentName(),
        snapshot.modulesVisited,
        snapshot.normalImportDescriptors,
        snapshot.delayImportDescriptors,
        snapshot.xinputSlotsPatched,
        snapshot.directInputSlotsPatched,
        snapshot.getProcAddressSlotsPatched,
        snapshot.dynamicXInputRedirects,
        snapshot.dynamicDirectInputRedirects,
        snapshot.patchFailures);
    WriteLogLine(config, message);

    wchar_t earlyMessage[512]{};
    std::swprintf(
        earlyMessage,
        std::size(earlyMessage),
        L"JoyShim 最小早期接管：XInput槽位=%u，DirectInput槽位=%u，GetProcAddress槽位=%u，失败=%u。该阶段仅处理主 EXE 按名称普通导入，不解析配置、不加载 SDL。",
        static_cast<unsigned int>(InterlockedCompareExchange(&g_earlyXInputPatched, 0, 0)),
        static_cast<unsigned int>(InterlockedCompareExchange(&g_earlyDirectInputPatched, 0, 0)),
        static_cast<unsigned int>(InterlockedCompareExchange(&g_earlyGetProcAddressPatched, 0, 0)),
        static_cast<unsigned int>(InterlockedCompareExchange(&g_earlyPatchFailures, 0, 0)));
    WriteLogLine(config, earlyMessage);

    if ((InterlockedCompareExchange(&g_hookMode, 0, 0) & HookDirectInput) != 0 &&
        g_entryKind != ProxyEntryKind::DInput8 &&
        snapshot.directInputSlotsPatched == 0 &&
        snapshot.getProcAddressSlotsPatched == 0) {
        WriteLogLine(
            config,
            L"警告：配置要求 DirectInput 去重，但当前尚未接管静态、延迟或动态 DirectInput8Create 路径；JoyShim 将继续监视后加载的游戏模块。");
    }
}

DWORD WINAPI RuntimeBootstrapWorker(void*) noexcept {
    // 本线程实际开始执行时已离开 PROCESS_ATTACH 的 loader lock。
    // 此前 DllMain 不得修改 IAT，也不得同步启动 SDL 或等待新线程。
    if (InterlockedCompareExchange(&g_detaching, 0, 0) != 0) return 0;
    ProxyEntryInstallEarlyHooks();
    // 给老游戏完成其他 DLL 的启动回调留出时间；期间真实 XInput
    // 导出仅返回未连接并触发幂等队列，不会抢在加载器锁内加载 SDL。
    Sleep(DeferredBootstrapDelayMs);
    if (InterlockedCompareExchange(&g_detaching, 0, 0) == 0) {
        EnsureRuntimeInitialized();
    }
    return 0;
}

}  // namespace

extern "C" FARPROC WINAPI JoyShimGetProcAddress(
    HMODULE module,
    LPCSTR nameOrOrdinal) noexcept {
    if (module != nullptr && nameOrOrdinal != nullptr) {
        wchar_t modulePath[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(module, modulePath, MAX_PATH);
        const wchar_t* moduleName =
            length != 0 && length < MAX_PATH ? BaseName(modulePath) : L"";
        if (IsXInputName(moduleName)) {
            FARPROC replacement = ResolveLocalXInputExport(nameOrOrdinal);
            if (replacement != nullptr) {
                InterlockedIncrement(&g_dynamicXInputRedirects);
                return replacement;
            }
        }
        if ((InterlockedCompareExchange(&g_hookMode, 0, 0) & HookDirectInput) != 0 &&
            EqualsIgnoreCase(moduleName, L"dinput8.dll") &&
            !IS_INTRESOURCE(nameOrOrdinal) &&
            std::strcmp(nameOrOrdinal, "DirectInput8Create") == 0) {
            InterlockedIncrement(&g_dynamicDirectInputRedirects);
            return FunctionToFarProc(&ConfiguredDirectInput8Create);
        }
    }
    return GetProcAddress(module, nameOrOrdinal);
}

void ProxyEntryProcessAttach(HMODULE module) noexcept {
    g_selfModule = module;
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length != 0 && length < MAX_PATH) {
        const wchar_t* name = BaseName(path);
        g_entryKind = ClassifyEntry(name);
        CopyWide(name, g_entryName, std::size(g_entryName));
    }

    wchar_t executable[MAX_PATH]{};
    const DWORD executableLength = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (executableLength != 0 && executableLength < MAX_PATH) {
        wchar_t* slash = std::wcsrchr(executable, L'\\');
        if (slash != nullptr) *slash = L'\0';
        CopyWide(executable, g_gameRoot, std::size(g_gameRoot));
    }
}

void ProxyEntryInstallEarlyHooks() noexcept {
    if (g_selfModule == nullptr || g_entryKind == ProxyEntryKind::Unknown ||
        InterlockedCompareExchange(&g_detaching, 0, 0) != 0) return;

    // 该调用只能在 loader lock 外执行（RuntimeBootstrapWorker）。
    // 早期阶段始终只处理主 EXE 的按名称普通导入。这样 winmm/
    // dinput8 可以在任何系统导出被调用前截住游戏的 XInput；XInput 入口
    // 也可以在游戏第一次创建 DirectInput 接口前截住 DirectInput8Create。
    // ConfiguredDirectInput8Create 会按实际配置决定是否隐藏设备，未启用规则时
    // 保持透明枚举。
    const LONG earlyMode = HookXInput | HookDirectInput | HookDynamicLookup;
    InterlockedExchange(&g_hookMode, earlyMode);
    HMODULE mainModule = GetModuleHandleW(nullptr);
    AcquireSRWLockExclusive(&g_patchLock);
    PatchModule(mainModule, earlyMode, false, false);
    ReleaseSRWLockExclusive(&g_patchLock);
    InterlockedExchange(
        &g_earlyXInputPatched,
        InterlockedCompareExchange(&g_xinputPatched, 0, 0));
    InterlockedExchange(
        &g_earlyDirectInputPatched,
        InterlockedCompareExchange(&g_directInputPatched, 0, 0));
    InterlockedExchange(
        &g_earlyGetProcAddressPatched,
        InterlockedCompareExchange(&g_getProcAddressPatched, 0, 0));
    InterlockedExchange(
        &g_earlyPatchFailures,
        InterlockedCompareExchange(&g_patchFailures, 0, 0));
}

void ProxyEntryQueueRuntimeBootstrap() noexcept {
    if (g_selfModule == nullptr || g_entryKind == ProxyEntryKind::Unknown) return;
    if (InterlockedCompareExchange(&g_bootstrapQueued, 1, 0) != 0) return;
    HANDLE thread = CreateThread(
        nullptr,
        0,
        RuntimeBootstrapWorker,
        nullptr,
        0,
        nullptr);
    if (thread != nullptr) {
        CloseHandle(thread);
    } else {
        // 无线程时保留透明的“未连接”返回，不在加载器锁内同步加载 SDL。
        InterlockedExchange(&g_bootstrapQueued, 0);
        OutputDebugStringW(
            L"JoyShim：无法创建无条件运行时启动线程，将在首次代理调用时重试。\n");
    }
}

void ProxyEntryConfigureHooks(
    const RuntimeConfig& config,
    bool blockDirectInput) noexcept {
    // 当前文件本身已经是 XInput 入口时，Bond.exe 的 XInput IAT 已由
    // Windows loader 直接解析到 JoyShim，不得再次改写同一调用链。
    // dinput8/winmm 才需要把宿主原有 XInput 导入转到 JoyShim。
    LONG mode = HookDynamicLookup;
    if (g_entryKind == ProxyEntryKind::DInput8 ||
        g_entryKind == ProxyEntryKind::WinMM) {
        mode |= HookXInput;
    }
    if (blockDirectInput && g_entryKind != ProxyEntryKind::DInput8) {
        mode |= HookDirectInput;
    }
    InterlockedExchange(&g_hookMode, mode);

    // 初始阶段只改写主程序普通 IAT，且此函数只能从第一次真实代理
    // 调用后的运行时初始化进入。后加载模块由 SDL 输入线程按节流扫描。
    AcquireSRWLockExclusive(&g_patchLock);
    PatchCurrentGameModules(false);
    ReleaseSRWLockExclusive(&g_patchLock);
    LogHookSnapshot(config, L"运行时接管复核（含最小早期接管）");
}

void ProxyEntryRefreshHooks(const RuntimeConfig& config) noexcept {
    if (InterlockedCompareExchange(&g_detaching, 0, 0) != 0) return;
    const ULONGLONG now = GetTickCount64();
    const LONG64 next = InterlockedCompareExchange64(
        &g_nextRefreshTick,
        0,
        0);
    if (next != 0 && now < static_cast<ULONGLONG>(next)) return;
    const LONG64 desired = static_cast<LONG64>(now + ModuleRefreshIntervalMs);
    if (InterlockedCompareExchange64(
            &g_nextRefreshTick,
            desired,
            next) != next) {
        return;
    }
    const ProxyHookSnapshot before = ProxyEntryHookSnapshot();
    AcquireSRWLockExclusive(&g_patchLock);
    PatchCurrentGameModules(true);
    ReleaseSRWLockExclusive(&g_patchLock);
    const ProxyHookSnapshot after = ProxyEntryHookSnapshot();
    if (after.xinputSlotsPatched != before.xinputSlotsPatched ||
        after.directInputSlotsPatched != before.directInputSlotsPatched ||
        after.getProcAddressSlotsPatched != before.getProcAddressSlotsPatched) {
        LogHookSnapshot(config, L"后加载模块");
    }
}

void ProxyEntryNotifyDirectInputCreate() noexcept {
    InterlockedIncrement(&g_directInputCreateCalls);
}

void ProxyEntryNotifyDirectInputDeviceHidden() noexcept {
    InterlockedIncrement(&g_directInputDevicesHidden);
}

void ProxyEntryProcessDetach() noexcept {
    InterlockedExchange(&g_detaching, 1);
}

HMODULE ProxyEntryModule() noexcept {
    return g_selfModule;
}

ProxyEntryKind ProxyEntryCurrentKind() noexcept {
    return g_entryKind;
}

const wchar_t* ProxyEntryCurrentName() noexcept {
    return g_entryName;
}

bool ProxyEntrySupportsDirectInputFilter() noexcept {
    return g_entryKind != ProxyEntryKind::Unknown;
}

ProxyHookSnapshot ProxyEntryHookSnapshot() noexcept {
    ProxyHookSnapshot snapshot{};
    snapshot.modulesVisited = static_cast<std::uint32_t>(
        InterlockedCompareExchange(&g_modulesVisited, 0, 0));
    snapshot.normalImportDescriptors = static_cast<std::uint32_t>(
        InterlockedCompareExchange(&g_normalDescriptors, 0, 0));
    snapshot.delayImportDescriptors = static_cast<std::uint32_t>(
        InterlockedCompareExchange(&g_delayDescriptors, 0, 0));
    snapshot.xinputSlotsPatched = static_cast<std::uint32_t>(
        InterlockedCompareExchange(&g_xinputPatched, 0, 0));
    snapshot.directInputSlotsPatched = static_cast<std::uint32_t>(
        InterlockedCompareExchange(&g_directInputPatched, 0, 0));
    snapshot.getProcAddressSlotsPatched = static_cast<std::uint32_t>(
        InterlockedCompareExchange(&g_getProcAddressPatched, 0, 0));
    snapshot.patchFailures = static_cast<std::uint32_t>(
        InterlockedCompareExchange(&g_patchFailures, 0, 0));
    snapshot.dynamicXInputRedirects = static_cast<std::uint32_t>(
        InterlockedCompareExchange(&g_dynamicXInputRedirects, 0, 0));
    snapshot.dynamicDirectInputRedirects = static_cast<std::uint32_t>(
        InterlockedCompareExchange(&g_dynamicDirectInputRedirects, 0, 0));
    snapshot.directInputCreateCalls = static_cast<std::uint32_t>(
        InterlockedCompareExchange(&g_directInputCreateCalls, 0, 0));
    snapshot.directInputDevicesHidden = static_cast<std::uint32_t>(
        InterlockedCompareExchange(&g_directInputDevicesHidden, 0, 0));
    return snapshot;
}

}  // namespace joyshim
