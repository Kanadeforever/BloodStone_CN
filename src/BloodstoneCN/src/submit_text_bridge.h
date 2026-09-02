#pragma once
#include "common.h"
#include "external_loc.h"
#include "text_coverage_resolver.h"

namespace bscn {
class SubmitTextBridge {
public:
    bool Prepare(HMODULE main_module, ExternalLocOverrides* overrides,
                 TextCoverageResolver* coverage, bool log_coverage_misses,
                 bool bypass_dynamic_scalars, bool lifecycle_probe);
    bool TryInstall();
    void Shutdown();
    bool installed() const { return installed_.load(); }
private:
    using SubmitTextFn = float (BSCN_THISCALL*)(
        void* manager, uint32_t context_a8, const float* position,
        uint32_t layer_a0, const wchar_t* text, const uint32_t* vertex_colors,
        float height_1bc, const void* style_list, float width_1b8,
        uint32_t context_ac);
    static float BSCN_FASTCALL HookSubmitText(
        void* manager, void*, uint32_t context_a8, const float* position,
        uint32_t layer_a0, const wchar_t* text,
        const uint32_t* vertex_colors, float height_1bc,
        const void* style_list, float width_1b8, uint32_t context_ac);
    float Forward(void* manager, uint32_t context_a8, const float* position,
                  uint32_t layer_a0, const wchar_t* text,
                  const uint32_t* vertex_colors, float height_1bc,
                  const void* style_list, float width_1b8,
                  uint32_t context_ac);
    void LogCoverageMiss(const wchar_t* text);
    const wchar_t* StabilizeEffectiveText(const wchar_t* replacement);

    static std::atomic<SubmitTextBridge*> active_bridge_;
    HMODULE main_module_ = nullptr;
    ExternalLocOverrides* overrides_ = nullptr;
    TextCoverageResolver* coverage_ = nullptr;
    uint8_t* submit_entry_ = nullptr;
    uint8_t* trampoline_ = nullptr;
    SubmitTextFn original_submit_text_ = nullptr;
    std::array<uint8_t,5> original_entry_bytes_{};
    std::mutex mutex_;
    std::atomic<bool> installed_{false};
    std::atomic<bool> waiting_logged_{false};
    std::array<std::atomic<bool>,11> mode_logged_{};
    bool log_coverage_misses_ = true;
    bool bypass_dynamic_scalars_ = true;
    bool lifecycle_probe_ = true;
    std::mutex stable_mutex_;
    std::map<std::wstring, std::wstring> stable_effective_texts_;
    std::atomic<bool> stable_buffer_logged_{false};
    std::atomic<bool> stable_buffer_limit_logged_{false};
    std::atomic<bool> font_gate_submit_skip_logged_{false};

    std::mutex miss_mutex_;
    std::unordered_set<uint64_t> logged_misses_;
    std::mutex hud_trace_mutex_;
    std::unordered_set<uint64_t> logged_hud_submit_states_;
    DWORD last_install_attempt_tick_ = 0;
};
}
