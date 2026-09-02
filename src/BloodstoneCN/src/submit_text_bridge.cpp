#include "submit_text_bridge.h"
#include "log.h"
#include "interop_host.h"

namespace bscn {
namespace {
constexpr uint32_t kSubmitTextStaticVa = 0x00E85900u;
constexpr size_t kTrampolineSize=10;
bool EncodeRelJump(uint8_t* at, const void* target, uint8_t out[5]) {
    if (!at || !target || !out) return false;
    const uint32_t next = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(at+5));
    const uint32_t dest = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(target));
    const uint32_t disp = dest-next;
    out[0]=0xE9; memcpy(out+1,&disp,4); return true;
}

bool IsAsciiSpaceUnit(wchar_t value) {
    return value == L' ' || value == L'\t' || value == L'\r' ||
           value == L'\n';
}


bool HasVisibleAsciiLetterOutsideMarkup(const wchar_t* text, size_t units) {
    if (!text) return false;
    for (size_t i = 0; i < units;) {
        // 完整动作/按键 token：~~{NAME}~~。token 名本身不算“残留英语”。
        if (i + 2u < units && text[i] == L'~' && text[i + 1u] == L'~' &&
            text[i + 2u] == L'{') {
            size_t close = i + 3u;
            while (close + 2u < units &&
                   !(text[close] == L'}' && text[close + 1u] == L'~' &&
                     text[close + 2u] == L'~')) {
                ++close;
            }
            if (close + 2u < units) {
                i = close + 3u;
                continue;
            }
        }
        // UIDialogue 某些阶段会把 wrapped token 展开成 {NAME}；同样只作为
        // 控制语法，不把 NAME 的 ASCII 字母当成可见英文正文。
        if (text[i] == L'{') {
            size_t close = i + 1u;
            while (close < units && text[close] != L'}') ++close;
            if (close < units && close > i + 1u) {
                i = close + 1u;
                continue;
            }
        }
        // printf 格式控制本身也不是可见英语，例如 `%d / %d 情报` 中的 d。
        // 只识别到标准转换字符才跳过，普通百分号保持原样。
        if (text[i] == L'%') {
            size_t end = i + 1u;
            while (end < units && end - i <= 24u) {
                const wchar_t c = text[end];
                if (c == L'%') {
                    end = i + 1u;
                    break;
                }
                const bool conversion =
                    c == L'd' || c == L'i' || c == L'u' || c == L'o' ||
                    c == L'x' || c == L'X' || c == L'f' || c == L'F' ||
                    c == L'e' || c == L'E' || c == L'g' || c == L'G' ||
                    c == L'a' || c == L'A' || c == L'c' || c == L'C' ||
                    c == L's' || c == L'S' || c == L'p' || c == L'n';
                if (conversion) {
                    i = end + 1u;
                    break;
                }
                ++end;
            }
            if (i > end) continue;
        }
        // 颜色/样式控制形如 ~ffffffff~。只在存在闭合 ~ 且 token 很短时跳过，
        // 避免吞掉普通正文中的波浪号。
        if (text[i] == L'~') {
            size_t close = i + 1u;
            while (close < units && close - i <= 24u && text[close] != L'~')
                ++close;
            if (close < units && text[close] == L'~' && close > i + 1u) {
                i = close + 1u;
                continue;
            }
        }
        const wchar_t c = text[i++];
        if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'))
            return true;
    }
    return false;
}

// 诊断日志不记录每帧变化的“纯数字 + 单位”HUD，例如 49m/143m。
// 这里只影响 TEXT_COVERAGE_MISS 日志，不参与实际文本解析或替换。
// 规则故意严格：至少一个数字，数字主体仅允许常见数值标点，最后只允许
// 一个 ASCII 字母单位，因此 Mission/MP7/按键名等普通文本不会被吞掉。
bool IsDynamicNumericUnitNoise(const wchar_t* text, size_t units) {
    if (!text || units < 2u) return false;
    size_t begin = 0;
    size_t end = units;
    while (begin < end && IsAsciiSpaceUnit(text[begin])) ++begin;
    while (end > begin && IsAsciiSpaceUnit(text[end - 1u])) --end;
    if (end - begin < 2u) return false;

    const wchar_t suffix = text[end - 1u];
    if (!((suffix >= L'A' && suffix <= L'Z') ||
          (suffix >= L'a' && suffix <= L'z'))) {
        return false;
    }

    bool saw_digit = false;
    for (size_t index = begin; index + 1u < end; ++index) {
        const wchar_t value = text[index];
        if (value >= L'0' && value <= L'9') {
            saw_digit = true;
            continue;
        }
        if (value == L'+' || value == L'-' || value == L'.' ||
            value == L',') {
            continue;
        }
        return false;
    }
    return saw_digit;
}

uint64_t HashHudTraceText(const wchar_t* text) {
    if (!text) return 0u;
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; text[i] && i < 512u; ++i) {
        const uint16_t unit = static_cast<uint16_t>(text[i]);
        hash ^= static_cast<uint8_t>(unit & 0xFFu);
        hash *= 1099511628211ull;
        hash ^= static_cast<uint8_t>((unit >> 8u) & 0xFFu);
        hash *= 1099511628211ull;
    }
    return hash;
}
}
std::atomic<SubmitTextBridge*> SubmitTextBridge::active_bridge_{nullptr};

bool SubmitTextBridge::Prepare(HMODULE main_module, ExternalLocOverrides* overrides,
                               TextCoverageResolver* coverage,
                               bool log_coverage_misses,
                               bool bypass_dynamic_scalars,
                               bool lifecycle_probe) {
    Shutdown();
    if (!main_module || !overrides || !coverage || !ValidateBondModule(main_module)) return false;
    main_module_=main_module; overrides_=overrides; coverage_=coverage; log_coverage_misses_=log_coverage_misses;
    bypass_dynamic_scalars_=bypass_dynamic_scalars; lifecycle_probe_=lifecycle_probe;
    submit_entry_=reinterpret_cast<uint8_t*>(RuntimeAddress(main_module_,kSubmitTextStaticVa));
    SubmitTextBridge* expected=nullptr;
    if (!active_bridge_.compare_exchange_strong(expected,this)) {
        main_module_=nullptr; overrides_=nullptr; coverage_=nullptr; submit_entry_=nullptr; return false;
    }
    BSCN_LOG_INFO("SubmitText bridge prepared; M2F1 owns the only 0x00E85900 hook");
    return true;
}

bool SubmitTextBridge::TryInstall() {
    if (installed_.load()) return true;
    const DWORD now=GetTickCount();
    if (last_install_attempt_tick_ && now-last_install_attempt_tick_<100) return false;
    last_install_attempt_tick_=now;
    std::lock_guard<std::mutex> lock(mutex_);
    if (installed_.load() || !submit_entry_ || active_bridge_.load()!=this) return installed_.load();
    static constexpr uint8_t sig[8]={0x51,0x0F,0x57,0xC0,0x55,0x56,0x57,0x8B};
    if (!BytesEqual(submit_entry_,sig,sizeof(sig))) {
        bool e=false;
        if (waiting_logged_.compare_exchange_strong(e,true))
            BSCN_LOG_INFO("SubmitText hook waiting for decrypted 0x00E85900");
        return false;
    }
    if (!ReadBytes(submit_entry_,original_entry_bytes_.data(),5)) return false;
    trampoline_=static_cast<uint8_t*>(VirtualAlloc(nullptr,kTrampolineSize,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    if (!trampoline_) return false;
    memcpy(trampoline_,original_entry_bytes_.data(),5);
    uint8_t back[5]{};
    if (!EncodeRelJump(trampoline_+5,submit_entry_+5,back)) { VirtualFree(trampoline_,0,MEM_RELEASE); trampoline_=nullptr; return false; }
    memcpy(trampoline_+5,back,5);
    FlushInstructionCache(GetCurrentProcess(),trampoline_,kTrampolineSize);
    DWORD old=0; VirtualProtect(trampoline_,kTrampolineSize,PAGE_EXECUTE_READ,&old);
    original_submit_text_=reinterpret_cast<SubmitTextFn>(trampoline_);
    uint8_t jump[5]{};
    if (!EncodeRelJump(submit_entry_,reinterpret_cast<void*>(&SubmitTextBridge::HookSubmitText),jump) ||
        !WriteCode(submit_entry_,jump,5)) {
        original_submit_text_=nullptr; VirtualFree(trampoline_,0,MEM_RELEASE); trampoline_=nullptr; return false;
    }
    installed_.store(true);
    BSCN_LOG_INFO("SUBMIT_TEXT_HOOK_OK entry=0x00E85900 owner=M2F1 integrated_text_coverage=1");
    return true;
}

float BSCN_FASTCALL SubmitTextBridge::HookSubmitText(
    void* manager, void*, uint32_t context_a8, const float* position,
    uint32_t layer_a0, const wchar_t* text, const uint32_t* vertex_colors,
    float height_1bc, const void* style_list, float width_1b8, uint32_t context_ac) {
    SubmitTextBridge* b=active_bridge_.load();
    return b ? b->Forward(manager,context_a8,position,layer_a0,text,vertex_colors,
                          height_1bc,style_list,width_1b8,context_ac) : 0.0f;
}

float SubmitTextBridge::Forward(
    void* manager, uint32_t context_a8, const float* position,
    uint32_t layer_a0, const wchar_t* text, const uint32_t* vertex_colors,
    float height_1bc, const void* style_list, float width_1b8, uint32_t context_ac) {
    if (!original_submit_text_) return 0.0f;
    // GetText 的 font gate 不能单独保证安全：R10.4 TextCoverage 仍可在
    // SubmitText 阶段把原英文重新翻成中文。Reset/首设备阶段 atlas 尚未
    // ready 时必须让 SubmitText 也保持游戏原文，否则中文会落到原生字体
    // 上并显示为口口口。
    if (overrides_ && !overrides_->rendering_ready()) {
        bool expected = false;
        if (font_gate_submit_skip_logged_.compare_exchange_strong(expected, true)) {
            BSCN_LOG_INFO("FONT_GATE_SKIP_SUBMIT action=KEEP_ORIGINAL until=ATLAS_READY");
        }
        interop::ObserveSubmittedText(text);
        return original_submit_text_(
            manager,context_a8,position,layer_a0,text,vertex_colors,
            height_1bc,style_list,width_1b8,context_ac);
    }
    const wchar_t* effective=text;
    const size_t original_units = text ? std::wcslen(text) : 0u;
    const bool dynamic_runtime_scalar =
        bypass_dynamic_scalars_ && text &&
        IsDynamicRuntimeScalar(text, original_units);
    if (dynamic_runtime_scalar && lifecycle_probe_) {
        const uint64_t key = HashHudTraceText(text) ^
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(text));
        bool log_it = false;
        {
            std::lock_guard<std::mutex> lock(hud_trace_mutex_);
            if (logged_hud_submit_states_.size() < 256u)
                log_it = logged_hud_submit_states_.insert(key).second;
        }
        if (log_it) {
            BSCN_LOG_INFO(
                "HUD_SUBMIT_TRACE source_ptr=%p effective_ptr=%p kind=DYNAMIC_SCALAR route=GAME_OWNED_BYPASS stable=SKIPPED text=%s",
                text, text, WideToUtf8(std::wstring(text, original_units)).c_str());
        }
    }
    if (overrides_ && text && !dynamic_runtime_scalar) {
        // R10 consolidated pipeline:
        //   1) original-key precise/deferred context
        //   2) deterministic source/template coverage
        // No UIDialogue post-object rewrite and no alternating 3-pass fixed point.
        // GetText now auto-defers only genuine printf/control-token templates.
        // Explicit BLI3 DEFER_TO_SUBMIT_TEXT records keep their original timing
        // path and are consumed here with lifecycle diagnostics.
        std::wstring current(text);
        std::wstring precise_copy;
        std::wstring coverage_scratch;
        bool changed_any=false;

        auto log_mode_once = [&](TextCoverageResolver::Mode mode) {
            const size_t mi=static_cast<size_t>(mode);
            if (mi>=mode_logged_.size()) return;
            bool expected=false;
            if (mode_logged_[mi].compare_exchange_strong(expected,true)) {
                BSCN_LOG_INFO(
                    "TEXT_COVERAGE_HIT mode=%s policy=DETERMINISTIC_SOURCE_ONLY",
                    TextCoverageResolver::ModeName(mode));
            }
        };
        auto different=[](const std::wstring& left,const wchar_t* right) {
            if (!right) return false;
            const size_t right_units=wcslen(right);
            return left.size()!=right_units ||
                   wmemcmp(left.data(),right,left.size())!=0;
        };

        uint32_t precise_units=0;
        if (const wchar_t* precise=overrides_->ResolveSubmittedText(
                current.c_str(),&precise_units)) {
            if (different(current,precise)) {
                precise_copy.assign(precise);
                current.swap(precise_copy);
                changed_any=true;
            }
        }

        // Two deterministic coverage passes are enough for a structured carrier
        // followed by one nested field (for example localized task carrier +
        // English objective tail). They never re-enter key-context resolution.
        for (unsigned pass=0;pass<2u;++pass) {
            if (!coverage_ || !coverage_->ready()) break;
            coverage_scratch.clear();
            const auto resolved=coverage_->Resolve(current.c_str(),&coverage_scratch);
            if (!resolved.text || !different(current,resolved.text)) break;
            current.assign(resolved.text);
            log_mode_once(resolved.mode);
            changed_any=true;
        }

        if (changed_any && current!=text) {
            effective=StabilizeEffectiveText(current.c_str());
            LogCoverageMiss(effective);
        } else {
            LogCoverageMiss(text);
        }
    }
    if (lifecycle_probe_ && text && *text &&
        (HasTechnicalIdentityToken(text, std::wcslen(text)) ||
         (effective && HasTechnicalIdentityToken(effective, std::wcslen(effective))))) {
        const uint64_t key = HashHudTraceText(text) ^
            (HashHudTraceText(effective) << 1u) ^
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(text));
        bool log_it = false;
        {
            std::lock_guard<std::mutex> lock(hud_trace_mutex_);
            if (logged_hud_submit_states_.size() < 256u)
                log_it = logged_hud_submit_states_.insert(key).second;
        }
        if (log_it) {
            const bool content_changed = effective &&
                std::wcscmp(text, effective) != 0;
            BSCN_LOG_INFO(
                "HUD_SUBMIT_TRACE source_ptr=%p effective_ptr=%p kind=TECHNICAL_IDENTITY translated=%u source=%s effective=%s",
                text, effective, content_changed ? 1u : 0u,
                WideToUtf8(std::wstring(text)).c_str(),
                WideToUtf8(std::wstring(effective ? effective : L"" )).c_str());
        }
    }

    // Publish the exact text Bond is about to submit. NPCSubtitles uses only a
    // short-lived normalized fingerprint to suppress an overlay when the game
    // already owns the same story subtitle. This does not change translation,
    // coverage logging, or SubmitText arguments.
    interop::ObserveSubmittedText(effective);

    const float result = original_submit_text_(
        manager,context_a8,position,layer_a0,effective,vertex_colors,
        height_1bc,style_list,width_1b8,context_ac);

    return result;
}

const wchar_t* SubmitTextBridge::StabilizeEffectiveText(
    const wchar_t* replacement) {
    if (!replacement || !*replacement) return replacement;
    constexpr size_t kMaximumStableTexts = 32768u;

    std::lock_guard<std::mutex> lock(stable_mutex_);
    auto existing = stable_effective_texts_.find(replacement);
    if (existing != stable_effective_texts_.end()) {
        return existing->second.c_str();
    }
    if (stable_effective_texts_.size() >= kMaximumStableTexts) {
        bool expected = false;
        if (stable_buffer_limit_logged_.compare_exchange_strong(
                expected, true)) {
            BSCN_LOG_ERROR(
                "SUBMIT_TEXT_STABLE_BUFFER_LIMIT entries=32768 action=USE_CALLER_BUFFER");
        }
        return replacement;
    }

    const std::wstring value(replacement);
    auto inserted = stable_effective_texts_.emplace(value, value).first;
    bool expected = false;
    if (stable_buffer_logged_.compare_exchange_strong(expected, true)) {
        BSCN_LOG_INFO(
            "SUBMIT_TEXT_STABLE_BUFFER_ACTIVE mode=PROCESS_LIFETIME_MAP max_entries=32768");
    }
    return inserted->second.c_str();
}

void SubmitTextBridge::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (installed_.load() && submit_entry_ && trampoline_) {
        uint8_t expected[5]{};
        if (EncodeRelJump(submit_entry_,reinterpret_cast<void*>(&SubmitTextBridge::HookSubmitText),expected) &&
            BytesEqual(submit_entry_,expected,5))
            WriteCode(submit_entry_,original_entry_bytes_.data(),5);
    }
    installed_.store(false);
    original_submit_text_=nullptr;
    if (trampoline_) { VirtualFree(trampoline_,0,MEM_RELEASE); trampoline_=nullptr; }
    original_entry_bytes_.fill(0);
    SubmitTextBridge* expected=this; active_bridge_.compare_exchange_strong(expected,nullptr);
    main_module_=nullptr; overrides_=nullptr; coverage_=nullptr; submit_entry_=nullptr;
    for (auto& v:mode_logged_) v.store(false);
    {
        std::lock_guard<std::mutex> stable_lock(stable_mutex_);
        stable_effective_texts_.clear();
    }
    stable_buffer_logged_.store(false);
    stable_buffer_limit_logged_.store(false);
    font_gate_submit_skip_logged_.store(false);
    {
        std::lock_guard<std::mutex> hud_lock(hud_trace_mutex_);
        logged_hud_submit_states_.clear();
    }
    bypass_dynamic_scalars_ = true;
    lifecycle_probe_ = true;
    {
        std::lock_guard<std::mutex> miss_lock(miss_mutex_);
        logged_misses_.clear();
    }
}

void SubmitTextBridge::LogCoverageMiss(const wchar_t* text) {
    if (!log_coverage_misses_ || !text || !*text) return;
    size_t units = 0;
    uint64_t hash = 1469598103934665603ull;
    while (text[units] && units < 16384u) {
        const uint16_t unit = static_cast<uint16_t>(text[units]);
        hash ^= static_cast<uint8_t>(unit & 0xFFu);
        hash *= 1099511628211ull;
        hash ^= static_cast<uint8_t>((unit >> 8u) & 0xFFu);
        hash *= 1099511628211ull;
        ++units;
    }
    if (units == 0u || units >= 16384u ||
        !HasVisibleAsciiLetterOutsideMarkup(text, units)) return;
    if (IsDynamicNumericUnitNoise(text, units)) return;
    bool inserted = false;
    size_t total = 0;
    {
        std::lock_guard<std::mutex> lock(miss_mutex_);
        if (logged_misses_.size() >= 512u) return;
        inserted = logged_misses_.insert(hash).second;
        total = logged_misses_.size();
    }
    if (!inserted) return;
    const size_t preview_units = std::min<size_t>(units, 220u);
    const std::string preview = WideToUtf8(std::wstring(text, preview_units));
    BSCN_LOG_INFO(
        "TEXT_COVERAGE_MISS unique=%u units=%u preview=%s",
        static_cast<unsigned>(total), static_cast<unsigned>(units),
        preview.c_str());
}
}
