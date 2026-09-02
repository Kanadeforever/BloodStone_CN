#include "common.h"
#include "config.h"
#include "dialogue_hook.h"
#include "dialogue_index.h"
#include "english_runtime_resolver.h"
#include "font_atlas.h"
#include "log.h"
#include "native_font_capture.h"
#include "renderer.h"
#include "resource_mode.h"

namespace npcsub {
namespace {
HMODULE g_self=nullptr;
std::atomic<bool> g_stop{false};
Config g_config{};
FontAtlas g_atlas;
DialogueIndex g_index;
EnglishRuntimeResolver g_english;
NativeFontCapture g_native_font;
SubtitleRenderer g_renderer;
DialogueHook g_dialogue;

DWORD WINAPI Worker(void*) {
    static_assert(sizeof(void*)==4,"NPCSubtitles must be built as Win32/x86");
    const std::wstring dir=ModuleDirectory(g_self);
    g_config=LoadConfig(dir);
    LogOpen(dir,g_config.logging);
    NPC_LOG_INFO("NPCSubtitles 0.2.2 D3D9InteropSafe starting independent=1 battle_subtitles=sealed");
    if(!g_config.enable){NPC_LOG_INFO("NPCSubtitles disabled by INI");return 0;}
    HMODULE main=GetModuleHandleW(nullptr);
    if(!ValidateBondModule(main)){NPC_LOG_ERROR("Unsupported Bond.exe fingerprint timestamp/image-size mismatch");return 0;}
    if(g_config.battle_subtitles)NPC_LOG_WARN("BattleSubtitles=1 ignored; battle path is permanently sealed for this branch");

    ResourceDecision decision=DecideResources(dir,g_config);
    ResourceMode mode=decision.mode;

    // Install the D3D9 chain before the expensive 22-file dialogue index scan.
    // 0.1/0.2 installed it after the scan and could miss Bond's real CreateDevice.
    // For Chinese mode we only need the BFA CPU atlas loaded first; the index can
    // be built afterwards.  DrawChinese lazily creates the GPU texture on the
    // real device, so early EndScene activity is safe while no subtitle is queued.
    bool chinese_atlas_ready=false;
    if(mode==ResourceMode::ChineseBscn){
        chinese_atlas_ready=g_atlas.Load(decision.bfa_path);
        if(!chinese_atlas_ready){NPC_LOG_WARN("CHINESE_BFA_INIT_FAILED action=FALLBACK_ENGLISH_NATIVE");g_atlas.Shutdown();mode=ResourceMode::EnglishNative;}
    }
    if(mode==ResourceMode::EnglishNative){
        if(!g_native_font.Prepare(main))NPC_LOG_WARN("NATIVE_FONT_CAPTURE_PREPARE_FAILED will_retry_not_possible");
    }
    NPC_LOG_INFO("NPC_SUBTITLE_RENDERER_EARLY_INSTALL mode=%s before_dialogue_index=1",ResourceModeName(mode));
    if(!g_renderer.Prepare(main,g_config,mode,&g_atlas,&g_native_font)||!g_renderer.Install()){NPC_LOG_ERROR("RENDERER_INIT_FAILED");return 0;}

    bool chinese_ready=false;
    if(mode==ResourceMode::ChineseBscn){
        chinese_ready=chinese_atlas_ready&&g_index.Initialize(dir,g_config);
        if(!chinese_ready){
            // Do not silently switch the already-live renderer to another mode.
            // A valid BFA with a failed dialogue index cannot produce Chinese NPC
            // subtitles anyway; fail closed instead of introducing a render-thread
            // mode race during initialization.
            NPC_LOG_ERROR("CHINESE_DIALOGUE_INDEX_INIT_FAILED action=DISABLE_PLUGIN renderer_chain_kept_safe=1");
            return 0;
        }
    }
    NPC_LOG_INFO("NPC_SUBTITLE_MODE selected=%s chinese_index=%u",ResourceModeName(mode),unsigned(g_index.size()));
    if(!g_dialogue.Prepare(main,g_config,mode,&g_index,&g_english,&g_renderer)||!g_dialogue.Install()){NPC_LOG_ERROR("DIALOGUE_INIT_FAILED");return 0;}
    NPC_LOG_INFO("NPC_SUBTITLE_INIT_DONE formal_asi=NPCSubtitles.asi log=NPCSubtitles.log");
    while(!g_stop.load()){
        if(mode==ResourceMode::EnglishNative&&!g_native_font.installed())g_native_font.TryInstall();
        Sleep(100);
    }
    return 0;
}
}
}

BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){npcsub::g_self=instance;DisableThreadLibraryCalls(instance);HANDLE h=CreateThread(nullptr,0,&npcsub::Worker,nullptr,0,nullptr);if(h)CloseHandle(h);}
    else if(reason==DLL_PROCESS_DETACH){npcsub::g_stop.store(true);}
    return TRUE;
}
