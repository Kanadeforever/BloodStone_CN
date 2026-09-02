#include "loc_text_bridge.h"
#include "log.h"

namespace bscn {
namespace {
constexpr uint32_t kGetTextStaticVa = 0x00E87740u;
constexpr uint32_t kGetTextSlotStaticVas[3] = {
    0x01179FC0u, 0x0117A690u, 0x0117A7D8u};
constexpr size_t kTextHashOffset = 0x190u;
LocTextBridge* g_loc_bridge = nullptr;
}

bool LocTextBridge::Prepare(HMODULE main_module,
                            ExternalLocOverrides* overrides,
                            bool lifecycle_probe) {
    Shutdown();
    if (!main_module || !overrides || !overrides->override_count() ||
        g_loc_bridge) return false;
    if (!ValidateBondModule(main_module)) return false;
    main_module_ = main_module;
    overrides_ = overrides;
    lifecycle_probe_ = lifecycle_probe;
    original_get_text_ = reinterpret_cast<GetTextFn>(
        RuntimeAddress(main_module_, kGetTextStaticVa));
    g_loc_bridge = this;
    BSCN_LOG_INFO("Unicode LOC bridge prepared overrides=%u selection=SOURCE_FIRST_HASH_SECOND",
                  static_cast<unsigned>(overrides_->override_count()));
    return true;
}

bool LocTextBridge::TryInstall() {
    if (installed_.load()) return true;
    const DWORD now = GetTickCount();
    if (last_install_attempt_tick_ && now-last_install_attempt_tick_ < 100)
        return false;
    last_install_attempt_tick_ = now;
    std::lock_guard<std::mutex> lock(mutex_);
    if (installed_.load() || !main_module_ || !overrides_ ||
        !original_get_text_ || g_loc_bridge != this)
        return installed_.load();
    static constexpr uint8_t sig[8] = {
        0x56,0x8B,0xF1,0x8B,0x86,0xC0,0x01,0x00};
    void* expected = reinterpret_cast<void*>(original_get_text_);
    bool ready = BytesEqual(expected, sig, sizeof(sig));
    for (uint32_t va : kGetTextSlotStaticVas) {
        void* observed = nullptr;
        auto** slot = reinterpret_cast<void**>(RuntimeAddress(main_module_, va));
        ready = ready && ReadBytes(slot, &observed, sizeof(observed)) &&
                observed == expected;
    }
    if (!ready) {
        bool e=false;
        if (waiting_logged_.compare_exchange_strong(e,true))
            BSCN_LOG_INFO("Unicode LOC hook waiting for decrypted GetText");
        return false;
    }
    void* replacement = reinterpret_cast<void*>(&LocTextBridge::HookGetText);
    patched_slot_count_ = 0;
    for (uint32_t va : kGetTextSlotStaticVas) {
        auto** slot = reinterpret_cast<void**>(RuntimeAddress(main_module_, va));
        if (!PatchPointer(slot, expected, replacement)) {
            for (size_t n=patched_slot_count_;n>0;--n)
                RestorePointer(patched_slots_[n-1].address, replacement,
                               patched_slots_[n-1].original);
            patched_slot_count_=0;
            return false;
        }
        patched_slots_[patched_slot_count_++] = {slot, expected};
    }
    installed_.store(true);
    BSCN_LOG_INFO(
        "UNICODE_LOC_HOOK_OK get_text=0x00E87740 slots=3 overrides=%u selection=SOURCE_FIRST_HASH_SECOND PAK_fallback=1",
        static_cast<unsigned>(overrides_->override_count()));
    return true;
}

const wchar_t* LocTextBridge::Resolve(void* self) {
    if (!original_get_text_) return nullptr;
    const wchar_t* original = original_get_text_(self);
    if (!self || !overrides_) return original;
    uint32_t hashes[2]{};
    if (!ReadBytes(static_cast<const uint8_t*>(self)+kTextHashOffset,
                   hashes,sizeof(hashes))) return original;

    // R10: native source is authoritative for identity. The two runtime hashes
    // are context discriminators only after the exact source family is known.
    // This makes stale/reused HUD hashes incapable of selecting another source.
    const wchar_t* replacement=overrides_->Resolve(hashes[1],hashes[0],original);
    const wchar_t* returned =
        (!replacement || replacement == original) ? original : replacement;
    TraceHud(self, original, returned, hashes[1], hashes[0]);
    if (returned == original) return original;

    bool e=false;
    if (first_hit_logged_.compare_exchange_strong(e,true)) {
        BSCN_LOG_INFO(
            "UNICODE_LOC_FIRST_HIT primary=0x%08X secondary=0x%08X units=%u selection=SOURCE_FIRST_HASH_SECOND",
            hashes[1],hashes[0],static_cast<unsigned>(wcslen(returned)));
    }
    return returned;
}

void LocTextBridge::TraceHud(void* self, const wchar_t* original,
                             const wchar_t* returned, uint32_t primary,
                             uint32_t secondary) {
    if (!lifecycle_probe_ || !self || !original || !*original || !returned) return;
    const size_t units = std::wcslen(original);
    if (!IsDynamicRuntimeScalar(original, units) &&
        !HasTechnicalIdentityToken(original, units)) {
        return;
    }
    const std::wstring source(original, units);
    const uintptr_t key = reinterpret_cast<uintptr_t>(self);
    HudTraceState previous;
    bool had_previous = false;
    bool changed = true;
    {
        std::lock_guard<std::mutex> lock(hud_trace_mutex_);
        const auto found = hud_trace_states_.find(key);
        if (found != hud_trace_states_.end()) {
            previous = found->second;
            had_previous = true;
            changed = previous.original_ptr != original ||
                      previous.returned_ptr != returned ||
                      previous.primary != primary ||
                      previous.secondary != secondary ||
                      previous.source != source;
        } else if (hud_trace_states_.size() >= 128u) {
            return;
        }
        if (!changed) return;
        hud_trace_states_[key] =
            {original, returned, primary, secondary, source};
    }
    const bool scalar = IsDynamicRuntimeScalar(original, units);
    BSCN_LOG_INFO(
        "HUD_GETTEXT_TRACE self=%p original_ptr=%p returned_ptr=%p primary=0x%08X secondary=0x%08X kind=%s source=%s pointer_owner=%s",
        self, original, returned, primary, secondary,
        scalar ? "DYNAMIC_SCALAR" : "TECHNICAL_IDENTITY",
        WideToUtf8(source).c_str(),
        returned == original ? "GAME" : "PLUGIN");
    if (had_previous && previous.source != source) {
        BSCN_LOG_INFO(
            "HUD_GETTEXT_SELF_TRANSITION self=%p old_source=%s new_source=%s old_ptr=%p new_ptr=%p",
            self, WideToUtf8(previous.source).c_str(),
            WideToUtf8(source).c_str(), previous.original_ptr, original);
    }
}

const wchar_t* BSCN_THISCALL LocTextBridge::HookGetText(void* self) {
    return g_loc_bridge ? g_loc_bridge->Resolve(self) : nullptr;
}

void LocTextBridge::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    void* replacement=reinterpret_cast<void*>(&LocTextBridge::HookGetText);
    for (size_t n=patched_slot_count_;n>0;--n)
        RestorePointer(patched_slots_[n-1].address,replacement,
                       patched_slots_[n-1].original);
    patched_slot_count_=0;
    installed_.store(false);
    waiting_logged_.store(false);
    first_hit_logged_.store(false);
    { std::lock_guard<std::mutex> trace_lock(hud_trace_mutex_); hud_trace_states_.clear(); }
    lifecycle_probe_ = true;
    if (g_loc_bridge==this) g_loc_bridge=nullptr;
    main_module_=nullptr;
    overrides_=nullptr;
    original_get_text_=nullptr;
}
}
