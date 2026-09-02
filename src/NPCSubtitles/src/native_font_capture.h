#pragma once
#include "common.h"
namespace npcsub {
#pragma pack(push,4)
struct NativeGlyphSnapshot {
    void* vtable = nullptr;
    uint16_t codepoint = 0;
    uint16_t pad06 = 0;
    float field08 = 0, field0c = 0, field10 = 0;
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    void* owner_font = nullptr;
};
#pragma pack(pop)
struct NativeRenderContext {
    void* font = nullptr;
    uint32_t variant = 0;
    float lookup_scale = 1.0f;
    uint16_t resource_handle = 0;
    IDirect3DTexture9* texture = nullptr;
    UINT texture_width = 0, texture_height = 0;
    bool valid() const { return font && texture && texture_width && texture_height; }
};
struct NativeRenderGlyph {
    float advance = 0, width = 0, height = 0;
    float u0=0,v0=0,u1=0,v1=0;
};
class NativeFontCapture {
public:
    bool Prepare(HMODULE main_module);
    bool TryInstall();
    NativeRenderContext SnapshotContext() const;
    bool LookupGlyph(const NativeRenderContext& context, wchar_t ch, NativeRenderGlyph* out) const;
    void InvalidateGpuResources();
    void Shutdown();
    bool installed() const { return installed_.load(); }
private:
    using GlyphLookupFn = void* (NPC_THISCALL*)(void*,uint16_t,uint32_t,float);
    using ResourceResolveFn = void* (NPC_THISCALL*)(void*,uint32_t);
    struct FontCandidate { void* font=nullptr; uint16_t handle=0; uint32_t variant=0; float scale=1; DWORD tick=0; };
    void* OnGlyph(void* font,uint16_t cp,uint32_t variant,float scale);
    void* OnResource(void* resolver,uint32_t key);
    static void* NPC_FASTCALL HookGlyph(void* font,void*,uint16_t cp,uint32_t variant,float scale);
    static void* NPC_FASTCALL HookResource(void* resolver,void*,uint32_t key);

    HMODULE main_=nullptr;
    void** glyph_slot_=nullptr;
    uint8_t* resource_call_=nullptr;
    GlyphLookupFn previous_glyph_=nullptr;
    ResourceResolveFn previous_resource_=nullptr;
    int32_t previous_resource_disp_=0;
    mutable std::mutex mutex_;
    std::vector<FontCandidate> fonts_;
    std::unordered_map<uint16_t,IDirect3DTexture9*> textures_;
    std::atomic<bool> installed_{false};
    DWORD last_try_=0;
};
}
