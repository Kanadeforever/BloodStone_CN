#pragma once
#include "common.h"
#include "config.h"
#include "dialogue_index.h"
#include "english_runtime_resolver.h"
#include "renderer.h"
#include "resource_mode.h"
namespace npcsub {
class DialogueHook {
public:
    bool Prepare(HMODULE main,const Config& config,ResourceMode mode,const DialogueIndex* index,EnglishRuntimeResolver* english,SubtitleRenderer* renderer);
    bool Install();
    void Shutdown();
private:
    using LookupFn=void* (NPC_CDECL*)(uint32_t);
    static void* NPC_CDECL HookLookup(uint32_t hash);
    void* OnLookup(uint32_t hash);
    HMODULE main_=nullptr;Config config_{};ResourceMode mode_=ResourceMode::EnglishNative;const DialogueIndex* index_=nullptr;EnglishRuntimeResolver* english_=nullptr;SubtitleRenderer* renderer_=nullptr;
    uint8_t* callsite_=nullptr;LookupFn previous_=nullptr;int32_t previous_disp_=0;std::atomic<bool> installed_{false};
};
}
