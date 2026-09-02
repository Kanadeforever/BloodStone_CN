#include "common.h"
#include "config.h"
#include "external_loc.h"
#include "font_atlas.h"
#include "hooks.h"
#include "loc_text_bridge.h"
#include "log.h"
#include "native_font_bridge.h"
#include "submit_text_bridge.h"
#include "text_coverage_resolver.h"

namespace bscn {
static HMODULE g_plugin=nullptr;
static std::atomic<bool> g_stopping{false};

class App {
public:
    bool Initialize() {
        const std::wstring dir=ModuleDirectory(g_plugin);
        const std::wstring ini=JoinPath(dir,L"BloodstoneCN.ini");
        config_=LoadConfig(ini);
        if (config_.logging) {
            const bool log_opened=g_log.Open(JoinPath(dir,L"BloodstoneCN.log"));
            if (log_opened) {
                BSCN_LOG_INFO("LOG_OPEN live_read=1 share=READ|WRITE|DELETE write=single_line");
            }
        }
        BSCN_LOG_INFO("BloodstoneCN M2F1+T3 R10.4 W1.2b CoverageRollback PerfSafe starting");
        if (!config_.enable) return true;

        HMODULE bond=GetModuleHandleW(nullptr);
        if (!ValidateBondModule(bond)) {
            BSCN_LOG_ERROR("Unsupported Bond.exe fingerprint timestamp/image-size");
            return false;
        }
        if (!atlas_.LoadCpuResources(dir,config_)) return !config_.require_atlas;

        // W1 实机日志证明：LOC/BLI/TextCoverage 在本机本轮花了约 1.2 秒，
        // 已晚于 Bond 的 Direct3DCreate9/CreateDevice。结果只有 BFA_CPU_OK，
        // 永远没有 BFA_GPU_OK/FONT_GATE_READY。W1.1 因此先抓住 D3D 生命周期，
        // 再做任何重型文本索引；R10.4 文本选择算法本身不在这里改动。
        if (config_.enable_native_glyph && !native_.Prepare(bond,&atlas_)) return false;
        if (!hooks::Install(bond,&atlas_,&native_,&external_loc_,&loc_,&submit_,
                            config_.enable_native_glyph)) return false;
        BSCN_LOG_INFO("R10_4_W1_2b_EARLY_D3D_READY stage=BEFORE_LOC_BLI_COVERAGE_PARSE atomic_chain=1");

        if (!external_loc_.Initialize(dir,config_)) return false;
        if (config_.enable_text_coverage && config_.enable_external_loc) {
            if (!coverage_.Initialize(external_loc_.index_path(), &external_loc_,
                                      config_.log_action_tokens)) {
                BSCN_LOG_ERROR("TextCoverage failed to read external BLI3");
                return false;
            }
            const auto& s=coverage_.stats();
            BSCN_LOG_INFO(
                "TextCoverage ready records=%u unique=%u ambiguous=%u authoritative_loc=%u skipped_ambiguous_loc=%u rejected_partial_english=%u canonical=%u canonical_ambiguous=%u safe_all_printf=%u all_printf_candidates=%u short_aliases=%u base_aliases=%u normalized=%u normalized_ambiguous=%u printf=%u tutorial=%u subtitle_bodies=%u subtitle_ambiguous=%u speaker_aliases=%u speaker_ambiguous=%u",
                static_cast<unsigned>(s.records),
                static_cast<unsigned>(s.unique_sources),
                static_cast<unsigned>(s.ambiguous_sources),
                static_cast<unsigned>(s.authoritative_loc_sources),
                static_cast<unsigned>(s.skipped_ambiguous_loc_sources),
                static_cast<unsigned>(s.rejected_partial_english_sources),
                static_cast<unsigned>(s.canonical_sources),
                static_cast<unsigned>(s.ambiguous_canonical_sources),
                static_cast<unsigned>(s.safe_all_record_printf_templates),
                static_cast<unsigned>(s.all_record_printf_candidates),
                static_cast<unsigned>(s.prefix_alias_sources),
                static_cast<unsigned>(s.base_label_alias_sources),
                static_cast<unsigned>(s.unique_normalized_sources),
                static_cast<unsigned>(s.ambiguous_normalized_sources),
                static_cast<unsigned>(s.printf_templates),
                static_cast<unsigned>(s.tutorial_templates),
                static_cast<unsigned>(s.subtitle_bodies),
                static_cast<unsigned>(s.ambiguous_subtitle_bodies),
                static_cast<unsigned>(s.speaker_aliases),
                static_cast<unsigned>(s.ambiguous_speaker_aliases));
        }


        // ExternalLoc::Initialize() 会重置 gate。重新同步早期 D3D 当前状态。
        // 只有 GPU atlas + NativeGlyph 都已成立时才允许任何中文输出。
        external_loc_.SetRenderingReady(
            !config_.enable_native_glyph || hooks::FontRenderReady());

        if (config_.enable_external_loc &&
            !loc_.Prepare(bond,&external_loc_,config_.hud_lifecycle_probe)) return false;
        if (config_.enable_external_loc &&
            !submit_.Prepare(bond,&external_loc_,&coverage_,
                             config_.log_coverage_misses,
                             config_.hud_bypass_dynamic_scalars,
                             config_.hud_lifecycle_probe)) return false;

        BSCN_LOG_INFO(
            "HOOK_CONSOLIDATION active_text_hooks=2 gettext=0x00E87740 submit=0x00E85900 npc_subtitles=EXTERNAL_ASI d3d9=INTEROP_HOST uidialogue=RETIRED text_path_probe=RETIRED");
        BSCN_LOG_INFO(
            "COVERAGE_MODE selection=SOURCE_FIRST_HASH_SECOND short_code_guard=RETIRED auto_defer_dialogue=0 auto_defer_dynamic=1 explicit_cg_defer=BLI3_ONLY font_gate=%u goal=LOC_EXISTING_TEXT_100_PERCENT",
            config_.enable_native_glyph ? 1u : 0u);
        BSCN_LOG_INFO(
            "R10_4_W1_2b_RUNTIME_CONTRACT dynamic_scalars=%s technical_identity_delivery=%s lifecycle_probe=%u source_first_hash_second=UNCHANGED text_semantics=W1_1_EXACT diagnostics=LOW_FREQUENCY_ONLY chinese_regression=FORBIDDEN early_d3d=1 submit_font_gate=1",
            config_.hud_bypass_dynamic_scalars ? "GAME_OWNED_BYPASS" : "R10_4_ORIGINAL",
            config_.hud_defer_technical_labels ? "GETTEXT_GAME_PTR_SUBMIT_TRANSLATE" : "R10_4_ORIGINAL",
            config_.hud_lifecycle_probe ? 1u : 0u);

        // Packing/decryption can finish after ASI initialization. One worker owns
        // all install retries. R10 intentionally does not patch EndScene just
        // to keep retrying hooks forever.
        const DWORD start=GetTickCount();
        while (!g_stopping.load() && GetTickCount()-start<60000u) {
            hooks::MaintenanceTick();
            const bool native_ready=!config_.enable_native_glyph || native_.installed();
            const bool text_ready=!config_.enable_external_loc ||
                                  (loc_.installed() && submit_.installed());
            const bool font_ready=!config_.enable_native_glyph || hooks::FontRenderReady();
            if (native_ready && text_ready && font_ready) break;
            Sleep(50);
        }
        BSCN_LOG_INFO(
            "Initialization state native=%u gettext=%u submit=%u font_render_ready=%u",
            native_.installed()?1u:0u,loc_.installed()?1u:0u,
            submit_.installed()?1u:0u,external_loc_.rendering_ready()?1u:0u);
        return true;
    }
private:
    Config config_{};
    ExternalFontAtlas atlas_;
    NativeFontBridge native_;
    ExternalLocOverrides external_loc_;
    TextCoverageResolver coverage_;
    LocTextBridge loc_;
    SubmitTextBridge submit_;
};

static DWORD WINAPI Worker(void*) {
    static App app;
    app.Initialize();
    return 0;
}
}

BOOL APIENTRY DllMain(HMODULE module,DWORD reason,LPVOID) {
    if (reason==DLL_PROCESS_ATTACH) {
        bscn::g_plugin=module;
        DisableThreadLibraryCalls(module);
        HANDLE thread=CreateThread(nullptr,0,&bscn::Worker,nullptr,0,nullptr);
        if (thread) CloseHandle(thread);
    } else if (reason==DLL_PROCESS_DETACH) {
        bscn::g_stopping.store(true);
    }
    return TRUE;
}
