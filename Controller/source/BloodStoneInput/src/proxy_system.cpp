#include "proxy_system.hpp"

#include "proxy_entry.hpp"

#include <array>
#include <cwchar>

namespace joyshim {
namespace {

struct SystemModuleState {
    INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    HMODULE module{};
    const wchar_t* name{};
};

SystemModuleState g_dinput8{INIT_ONCE_STATIC_INIT, nullptr, L"dinput8.dll"};
SystemModuleState g_winmm{INIT_ONCE_STATIC_INIT, nullptr, L"winmm.dll"};

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

SystemModuleState& StateFor(SystemDllKind kind) noexcept {
    if (kind == SystemDllKind::DInput8) return g_dinput8;
    return g_winmm;
}

}  // namespace

FARPROC ResolveSystemExport(SystemDllKind kind, const char* name) noexcept {
    // 系统 API 可能从其他 DLL 的 DllMain 中被调用。这里绝不直接执行
    // 配置解析、IAT 修补或 SDL 初始化；仅在第一次真实系统导出调用时
    // 排队启动线程并立即透明转发。DLL_PROCESS_ATTACH 本身不会抢跑。
    ProxyEntryQueueRuntimeBootstrap();
    SystemModuleState& state = StateFor(kind);
    InitOnceExecuteOnce(&state.once, LoadSystemModule, &state, nullptr);
    return state.module != nullptr && name != nullptr ? GetProcAddress(state.module, name) : nullptr;
}

}  // namespace joyshim
