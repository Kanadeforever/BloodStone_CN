#pragma once
#include "common.h"
#include "font_atlas.h"

namespace bscn {

class NativeFontBridge {
public:
    bool Prepare(HMODULE main_module, ExternalFontAtlas* atlas);
    bool TryInstall();
    void SetTexture(IDirect3DTexture9* texture);
    void Shutdown();
    bool installed() const { return installed_.load(); }
private:
    using GlyphLookupFn = void* (BSCN_THISCALL*)(void*, uint16_t, uint32_t, float);
    using ResourceResolveFn = void* (BSCN_THISCALL*)(void*, uint32_t);

    struct NativeGlyph {
        void* vtable = nullptr;       // +00
        uint16_t codepoint = 0;       // +04
        uint16_t reserved = 0;        // +06
        float field08 = 0.0f;         // +08
        float field0c = 0.0f;         // +0C
        float field10 = 0.0f;         // +10
        float u0 = 0.0f;              // +14
        float v0 = 0.0f;              // +18
        float u1 = 0.0f;              // +1C
        float v1 = 0.0f;              // +20
        void* owner_font = nullptr;   // +24
    };
    struct TextureResourceView {
        uint32_t reserved0 = 0;
        uint32_t reserved1 = 0;
        IDirect3DBaseTexture9* volatile texture = nullptr;
    };
    static_assert(sizeof(void*) == 4, "BloodstoneCN requires Win32/x86");
    static_assert(sizeof(NativeGlyph) == 0x28, "NativeGlyph ABI changed");
    static_assert(offsetof(NativeGlyph, owner_font) == 0x24, "NativeGlyph owner offset changed");
    static_assert(offsetof(TextureResourceView, texture) == 0x08, "Texture resource ABI changed");

    struct FontContext {
        std::array<uint8_t, 0x138> owner_shadow{};
        std::map<uint16_t, NativeGlyph> glyphs;
        uint16_t original_resource_handle = 0;
    };

    static void* BSCN_FASTCALL HookGlyphLookup(void* font, void*, uint16_t codepoint,
                                               uint32_t variant, float scale);
    static void* BSCN_FASTCALL HookResourceResolve(void* resolver, void*, uint32_t key);
    void* LookupGlyph(void* font, uint16_t codepoint, uint32_t variant, float scale);
    void* ResolveResource(void* resolver, uint32_t key);
    FontContext* GetOrCreateFontContext(void* font);

    HMODULE main_module_ = nullptr;
    ExternalFontAtlas* atlas_ = nullptr;
    void** glyph_lookup_slot_ = nullptr;
    uint8_t* resource_resolve_call_ = nullptr;
    GlyphLookupFn original_glyph_lookup_ = nullptr;
    ResourceResolveFn original_resource_resolve_ = nullptr;
    int32_t original_resource_call_displacement_ = 0;
    uintptr_t glyph_vtable_ = 0;
    TextureResourceView texture_resource_{};
    std::unordered_map<void*, std::unique_ptr<FontContext>> font_contexts_;
    std::unordered_set<uint16_t> logged_codepoints_;
    std::unordered_set<uint16_t> preserved_native_codepoints_;
    std::mutex mutex_;
    std::atomic<bool> installed_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> waiting_logged_{false};
    std::atomic<bool> texture_resolve_logged_{false};
    DWORD last_install_attempt_tick_ = 0;
};

}  // namespace bscn
