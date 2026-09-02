#include "native_font_capture.h"
#include "log.h"
namespace npcsub {
namespace {
constexpr uint32_t kGlyphLookupSlotStaticVa=0x011A02D8u;
constexpr uint32_t kResourceResolveCallStaticVa=0x00E42C21u;
constexpr size_t kOwnerResourceHandleOffset=0x12Cu;
NativeFontCapture* g_capture=nullptr;
}
static_assert(sizeof(void*)!=4 || sizeof(NativeGlyphSnapshot)==0x28,"Native glyph ABI mismatch");

bool NativeFontCapture::Prepare(HMODULE main_module){Shutdown();if(!main_module||!ValidateBondModule(main_module)||g_capture)return false;main_=main_module;glyph_slot_=reinterpret_cast<void**>(RuntimeAddress(main_,kGlyphLookupSlotStaticVa));resource_call_=reinterpret_cast<uint8_t*>(RuntimeAddress(main_,kResourceResolveCallStaticVa));g_capture=this;return true;}
bool NativeFontCapture::TryInstall(){if(installed_.load())return true;DWORD now=GetTickCount();if(last_try_&&now-last_try_<100)return false;last_try_=now;if(!main_||g_capture!=this)return false;
    uint8_t op=0;void* slot=nullptr;if(!ReadBytes(glyph_slot_,&slot,sizeof(slot))||!slot||!ReadBytes(resource_call_,&op,1)||op!=0xE8)return false;
    void* prev_glyph=nullptr;if(!PatchPointerChain(glyph_slot_,reinterpret_cast<void*>(&HookGlyph),&prev_glyph))return false;
    void* prev_resource=nullptr;int32_t disp=0;if(!PatchRelativeCall(resource_call_,reinterpret_cast<void*>(&HookResource),&prev_resource,&disp)){RestorePointerIfOwned(glyph_slot_,reinterpret_cast<void*>(&HookGlyph),prev_glyph);return false;}
    previous_glyph_=reinterpret_cast<GlyphLookupFn>(prev_glyph);previous_resource_=reinterpret_cast<ResourceResolveFn>(prev_resource);previous_resource_disp_=disp;installed_.store(true);
    NPC_LOG_INFO("NATIVE_FONT_CAPTURE_HOOK_OK glyph_slot=0x011A02D8 resource_call=0x00E42C21");return true;}
void* NativeFontCapture::OnGlyph(void* font,uint16_t cp,uint32_t variant,float scale){void* result=previous_glyph_?previous_glyph_(font,cp,variant,scale):nullptr;if(!font||cp<0x20||cp>0x7e)return result;uint16_t handle=0;if(!ReadBytes(static_cast<uint8_t*>(font)+kOwnerResourceHandleOffset,&handle,sizeof(handle))||!handle)return result;
    NativeGlyphSnapshot g{};if(!result||!ReadBytes(result,&g,sizeof(g))||g.codepoint!=cp)return result;std::lock_guard<std::mutex> lock(mutex_);DWORD now=GetTickCount();auto it=std::find_if(fonts_.begin(),fonts_.end(),[&](const FontCandidate& f){return f.font==font;});if(it==fonts_.end()){if(fonts_.size()>=16)fonts_.erase(fonts_.begin());fonts_.push_back({font,handle,variant,scale,now});}else{it->handle=handle;it->variant=variant;it->scale=scale;it->tick=now;}return result;}
void* NativeFontCapture::OnResource(void* resolver,uint32_t key){void* result=previous_resource_?previous_resource_(resolver,key):nullptr;if(!result)return result;IDirect3DTexture9* texture=nullptr;if(ReadBytes(static_cast<uint8_t*>(result)+8,&texture,sizeof(texture))&&texture){std::lock_guard<std::mutex> lock(mutex_);textures_[static_cast<uint16_t>(key)]=texture;}return result;}
void* NPC_FASTCALL NativeFontCapture::HookGlyph(void* font,void*,uint16_t cp,uint32_t variant,float scale){return g_capture?g_capture->OnGlyph(font,cp,variant,scale):nullptr;}
void* NPC_FASTCALL NativeFontCapture::HookResource(void* resolver,void*,uint32_t key){return g_capture?g_capture->OnResource(resolver,key):nullptr;}
NativeRenderContext NativeFontCapture::SnapshotContext() const {NativeRenderContext out{};if(!installed_.load())return out;std::lock_guard<std::mutex> lock(mutex_);if(fonts_.empty())return out;const FontCandidate* best=nullptr;for(const auto& f:fonts_){auto t=textures_.find(f.handle);if(t==textures_.end()||!t->second)continue;if(!best || f.tick > best->tick) best=&f;}if(!best)return out;auto ti=textures_.find(best->handle);out.font=best->font;out.variant=best->variant;out.lookup_scale=best->scale;out.resource_handle=best->handle;out.texture=ti->second;D3DSURFACE_DESC d{};if(FAILED(out.texture->GetLevelDesc(0,&d))){out.texture=nullptr;return out;}out.texture_width=d.Width;out.texture_height=d.Height;return out;}
void NativeFontCapture::InvalidateGpuResources(){std::lock_guard<std::mutex> lock(mutex_);textures_.clear();NPC_LOG_INFO("NPC_NATIVE_FONT_GPU_INVALIDATED");}
bool NativeFontCapture::LookupGlyph(const NativeRenderContext& c,wchar_t ch,NativeRenderGlyph* out) const {if(!out||!c.valid()||!previous_glyph_||ch<0x20||ch>0x7e)return false;void* p=previous_glyph_(c.font,uint16_t(ch),c.variant,c.lookup_scale);NativeGlyphSnapshot g{};if(!p||!ReadBytes(p,&g,sizeof(g))||g.codepoint!=uint16_t(ch))return false;out->u0=g.u0;out->v0=g.v0;out->u1=g.u1;out->v1=g.v1;out->advance=g.field08+g.field0c+g.field10;out->width=std::max(1.0f,(g.u1-g.u0)*c.texture_width);out->height=std::max(1.0f,(g.v1-g.v0)*c.texture_height);return true;}
void NativeFontCapture::Shutdown(){installed_.store(false);if(glyph_slot_&&previous_glyph_)RestorePointerIfOwned(glyph_slot_,reinterpret_cast<void*>(&HookGlyph),reinterpret_cast<void*>(previous_glyph_));if(resource_call_&&previous_resource_)RestoreRelativeCallIfOwned(resource_call_,reinterpret_cast<void*>(&HookResource),previous_resource_disp_);{std::lock_guard<std::mutex> lock(mutex_);fonts_.clear();textures_.clear();}glyph_slot_=nullptr;resource_call_=nullptr;previous_glyph_=nullptr;previous_resource_=nullptr;main_=nullptr;if(g_capture==this)g_capture=nullptr;}
}
