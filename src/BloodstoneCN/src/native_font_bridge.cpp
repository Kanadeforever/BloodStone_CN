#include "native_font_bridge.h"
#include "log.h"

namespace bscn {
namespace {
constexpr uint32_t kGlyphLookupSlotStaticVa = 0x011A02D8u;
constexpr uint32_t kResourceResolveCallStaticVa = 0x00E42C21u;
constexpr uint16_t kChineseTextureHandle = 0xBFFFu;
constexpr size_t kOwnerResourceHandleOffset = 0x12Cu;
NativeFontBridge* g_bridge = nullptr;
}

bool NativeFontBridge::Prepare(HMODULE main_module, ExternalFontAtlas* atlas) {
    Shutdown();
    if (!main_module || !atlas || !atlas->loaded() || g_bridge) return false;
    if (!ValidateBondModule(main_module)) {
        BSCN_LOG_ERROR("NativeGlyph refused: Bond.exe fingerprint mismatch");
        return false;
    }
    main_module_ = main_module;
    atlas_ = atlas;
    glyph_lookup_slot_ = reinterpret_cast<void**>(RuntimeAddress(main_module_, kGlyphLookupSlotStaticVa));
    resource_resolve_call_ = reinterpret_cast<uint8_t*>(RuntimeAddress(main_module_, kResourceResolveCallStaticVa));
    g_bridge = this;
    return true;
}

bool NativeFontBridge::TryInstall() {
    if (installed_.load()) return true;
    const DWORD now = GetTickCount();
    if (last_install_attempt_tick_ && now - last_install_attempt_tick_ < 100) return false;
    last_install_attempt_tick_ = now;
    std::lock_guard<std::mutex> lock(mutex_);
    if (installed_.load() || !main_module_ || !atlas_ || g_bridge != this) return installed_.load();

    void* observed = nullptr;
    // The vtable pointer itself is stable data; the resource CALL may still be encrypted.
    uint8_t call_opcode = 0;
    const bool slot_ready = ReadBytes(glyph_lookup_slot_, &observed, sizeof(observed)) && observed != nullptr;
    const bool call_ready = ReadBytes(resource_resolve_call_, &call_opcode, 1) && call_opcode == 0xE8;
    if (!slot_ready || !call_ready) {
        bool expected = false;
        if (waiting_logged_.compare_exchange_strong(expected, true))
            BSCN_LOG_INFO("NativeGlyph hook waiting for stable slot/call signature");
        return false;
    }

    original_glyph_lookup_ = reinterpret_cast<GlyphLookupFn>(observed);
    void* original_resolve = nullptr;
    int32_t old_disp = 0;
    if (!PatchPointer(glyph_lookup_slot_, observed,
                      reinterpret_cast<void*>(&NativeFontBridge::HookGlyphLookup))) {
        original_glyph_lookup_ = nullptr;
        return false;
    }
    if (!PatchRelativeCall(resource_resolve_call_,
                           reinterpret_cast<void*>(&NativeFontBridge::HookResourceResolve),
                           &original_resolve, &old_disp)) {
        RestorePointer(glyph_lookup_slot_,
                       reinterpret_cast<void*>(&NativeFontBridge::HookGlyphLookup), observed);
        original_glyph_lookup_ = nullptr;
        return false;
    }
    original_resource_resolve_ = reinterpret_cast<ResourceResolveFn>(original_resolve);
    original_resource_call_displacement_ = old_disp;
    installed_.store(true);
    active_.store(texture_resource_.texture != nullptr);
    BSCN_LOG_INFO("NATIVE_GLYPH_HOOK_OK lookup_slot=0x011A02D8 texture_call=0x00E42C21 handle=0xBFFF");
    return true;
}

void NativeFontBridge::SetTexture(IDirect3DTexture9* texture) {
    InterlockedExchangePointer(
        reinterpret_cast<void* volatile*>(&texture_resource_.texture), texture);
    active_.store(installed_.load() && texture != nullptr);
    texture_resolve_logged_.store(false);
}

NativeFontBridge::FontContext* NativeFontBridge::GetOrCreateFontContext(void* font) {
    auto found = font_contexts_.find(font);
    if (found != font_contexts_.end()) return found->second.get();
    auto context = std::make_unique<FontContext>();
    if (!ReadBytes(font, context->owner_shadow.data(), context->owner_shadow.size())) return nullptr;
    if (!ReadBytes(context->owner_shadow.data() + kOwnerResourceHandleOffset,
                   &context->original_resource_handle,
                   sizeof(context->original_resource_handle))) return nullptr;
    memcpy(context->owner_shadow.data() + kOwnerResourceHandleOffset,
           &kChineseTextureHandle, sizeof(kChineseTextureHandle));
    FontContext* raw = context.get();
    font_contexts_.emplace(font, std::move(context));
    BSCN_LOG_INFO("Created Chinese font shadow font=%p native_handle=0x%04X chinese_handle=0xBFFF",
                  font, raw->original_resource_handle);
    return raw;
}

void* NativeFontBridge::LookupGlyph(void* font, uint16_t codepoint,
                                    uint32_t variant, float scale) {
    GlyphLookupFn original = original_glyph_lookup_;
    if (!original) return nullptr;
    void* native = original(font, codepoint, variant, scale);
    if (!active_.load() || !atlas_ || !texture_resource_.texture || codepoint < 0x80u)
        return native;

    NativeGlyph native_snapshot{};
    const bool native_read = native && ReadBytes(native, &native_snapshot, sizeof(native_snapshot));
    if (native_read && native_snapshot.codepoint == codepoint) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (preserved_native_codepoints_.size() < 32 &&
            preserved_native_codepoints_.insert(codepoint).second)
            BSCN_LOG_INFO("NATIVE_GLYPH_NATIVE_PRESERVED U+%04X", codepoint);
        return native;
    }
    const GlyphInfo* source = atlas_->FindGlyphExact(static_cast<char32_t>(codepoint));
    if (!source) return native;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_.load() || !texture_resource_.texture) return native;
    FontContext* context = GetOrCreateFontContext(font);
    if (!context) return native;
    auto existing = context->glyphs.find(codepoint);
    if (existing != context->glyphs.end()) return &existing->second;

    if (!glyph_vtable_ && native_read) glyph_vtable_ = reinterpret_cast<uintptr_t>(native_snapshot.vtable);
    if (!glyph_vtable_) return native;

    NativeGlyph glyph{};
    glyph.vtable = reinterpret_cast<void*>(glyph_vtable_);
    glyph.codepoint = codepoint;
    // M2F1: the observed renderer advances by +08 + +0C + +10.
    // Never put BFA height into +10.  For a 36px glyph with advance 32:
    // 0 + 36 + (-4) = 32.
    glyph.field08 = static_cast<float>(source->bearing_x);
    glyph.field0c = static_cast<float>(source->width);
    glyph.field10 = static_cast<float>(source->advance_x - source->bearing_x - source->width);
    glyph.u0 = source->u0; glyph.v0 = source->v0;
    glyph.u1 = source->u1; glyph.v1 = source->v1;
    glyph.owner_font = context->owner_shadow.data();
    auto inserted = context->glyphs.emplace(codepoint, glyph);
    if (logged_codepoints_.size() < 64 && logged_codepoints_.insert(codepoint).second) {
        BSCN_LOG_INFO("NATIVE_GLYPH_M2F1 U+%04X field08=%.1f field0C=%.1f field10=%.1f sum=%.1f size=%dx%d",
                      codepoint, glyph.field08, glyph.field0c, glyph.field10,
                      glyph.field08 + glyph.field0c + glyph.field10,
                      source->width, source->height);
    }
    return &inserted.first->second;
}

void* NativeFontBridge::ResolveResource(void* resolver, uint32_t key) {
    const uint16_t handle = static_cast<uint16_t>(key);
    if (active_.load() && handle == kChineseTextureHandle) {
        if (!texture_resource_.texture) return nullptr;
        bool expected = false;
        if (texture_resolve_logged_.compare_exchange_strong(expected, true))
            BSCN_LOG_INFO("NATIVE_GLYPH_TEXTURE_OK handle=0xBFFF resource=%p texture=%p",
                          &texture_resource_, texture_resource_.texture);
        return &texture_resource_;
    }
    return original_resource_resolve_ ? original_resource_resolve_(resolver, key) : nullptr;
}

void* BSCN_FASTCALL NativeFontBridge::HookGlyphLookup(
    void* font, void*, uint16_t codepoint, uint32_t variant, float scale) {
    return g_bridge ? g_bridge->LookupGlyph(font, codepoint, variant, scale) : nullptr;
}
void* BSCN_FASTCALL NativeFontBridge::HookResourceResolve(void* resolver, void*, uint32_t key) {
    return g_bridge ? g_bridge->ResolveResource(resolver, key) : nullptr;
}

void NativeFontBridge::Shutdown() {
    active_.store(false);
    std::lock_guard<std::mutex> lock(mutex_);
    if (glyph_lookup_slot_ && original_glyph_lookup_)
        RestorePointer(glyph_lookup_slot_,
                       reinterpret_cast<void*>(&NativeFontBridge::HookGlyphLookup),
                       reinterpret_cast<void*>(original_glyph_lookup_));
    if (resource_resolve_call_ && original_resource_resolve_)
        RestoreRelativeCall(resource_resolve_call_,
                            reinterpret_cast<void*>(&NativeFontBridge::HookResourceResolve),
                            original_resource_call_displacement_);
    installed_.store(false);
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&texture_resource_.texture), nullptr);
    font_contexts_.clear();
    logged_codepoints_.clear(); preserved_native_codepoints_.clear();
    glyph_lookup_slot_ = nullptr; resource_resolve_call_ = nullptr;
    original_glyph_lookup_ = nullptr; original_resource_resolve_ = nullptr;
    atlas_ = nullptr; main_module_ = nullptr; glyph_vtable_ = 0;
    if (g_bridge == this) g_bridge = nullptr;
}

}  // namespace bscn
