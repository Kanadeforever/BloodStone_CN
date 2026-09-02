#pragma once
#include "common.h"
#include "external_loc.h"

namespace bscn {
class LocTextBridge {
public:
    bool Prepare(HMODULE main_module, ExternalLocOverrides* overrides,
                 bool lifecycle_probe);
    bool TryInstall();
    void Shutdown();
    bool installed() const { return installed_.load(); }
private:
    using GetTextFn = const wchar_t* (BSCN_THISCALL*)(void*);
    struct HudTraceState {
        const wchar_t* original_ptr = nullptr;
        const wchar_t* returned_ptr = nullptr;
        uint32_t primary = 0;
        uint32_t secondary = 0;
        std::wstring source;
    };
    void TraceHud(void* self, const wchar_t* original, const wchar_t* returned,
                  uint32_t primary, uint32_t secondary);
    struct PatchedSlot { void** address = nullptr; void* original = nullptr; };
    static const wchar_t* BSCN_THISCALL HookGetText(void* self);
    const wchar_t* Resolve(void* self);
    HMODULE main_module_ = nullptr;
    ExternalLocOverrides* overrides_ = nullptr;
    GetTextFn original_get_text_ = nullptr;
    std::array<PatchedSlot, 3> patched_slots_{};
    size_t patched_slot_count_ = 0;
    std::mutex mutex_;
    std::atomic<bool> installed_{false};
    std::atomic<bool> waiting_logged_{false};
    std::atomic<bool> first_hit_logged_{false};
    bool lifecycle_probe_ = true;
    std::mutex hud_trace_mutex_;
    std::unordered_map<uintptr_t, HudTraceState> hud_trace_states_;
    DWORD last_install_attempt_tick_ = 0;
};
}
