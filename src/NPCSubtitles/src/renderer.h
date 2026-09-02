#pragma once
#include "common.h"
#include "config.h"
#include "font_atlas.h"
#include "native_font_capture.h"
#include "resource_mode.h"
namespace npcsub {
class SubtitleRenderer {
public:
    bool Prepare(HMODULE main_module,const Config& config,ResourceMode mode,FontAtlas* atlas,NativeFontCapture* native_font);
    bool Install();
    void Enqueue(uint32_t hash,const std::wstring& text);
    void Shutdown();
    bool installed() const { return installed_.load(); }
private:
    struct Slot { uint32_t hash=0; DWORD start=0,expire=0; std::wstring text; };
    using Direct3DCreate9Fn=IDirect3D9* (WINAPI*)(UINT);
    using CreateDeviceFn=HRESULT (STDMETHODCALLTYPE*)(IDirect3D9*,UINT,D3DDEVTYPE,HWND,DWORD,D3DPRESENT_PARAMETERS*,IDirect3DDevice9**);
    using EndSceneFn=HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*);
    using ResetFn=HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*,D3DPRESENT_PARAMETERS*);
    static IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdk);
    static HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9* self,UINT adapter,D3DDEVTYPE type,HWND window,DWORD behavior,D3DPRESENT_PARAMETERS* pp,IDirect3DDevice9** out);
    static HRESULT STDMETHODCALLTYPE HookEndScene(IDirect3DDevice9* device);
    static HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* device,D3DPRESENT_PARAMETERS* pp);
    bool HookD3D9(IDirect3D9* d3d9,const char* source);
    bool BootstrapDeviceHooks();
    bool HookDevice(IDirect3DDevice9* device,bool initialize_resources=true);
    void BeforeReset(IDirect3DDevice9* device,const D3DPRESENT_PARAMETERS* pp);
    void AfterReset(IDirect3DDevice9* device,HRESULT hr);
    void Draw(IDirect3DDevice9* device);
    void DrawChinese(IDirect3DDevice9* device,const std::wstring& text,float y);
    void DrawEnglish(IDirect3DDevice9* device,const std::wstring& text,float y);

    HMODULE main_=nullptr; Config config_{}; ResourceMode mode_=ResourceMode::EnglishNative; FontAtlas* atlas_=nullptr; NativeFontCapture* native_font_=nullptr;
    Direct3DCreate9Fn previous_d3d9_=nullptr; CreateDeviceFn previous_create_=nullptr; EndSceneFn previous_end_scene_=nullptr; ResetFn previous_reset_=nullptr;
    void** create_slot_=nullptr; void** end_scene_slot_=nullptr; void** reset_slot_=nullptr;
    std::mutex slots_mutex_; std::array<Slot,4> slots_{};
    std::atomic<bool> installed_{false};
    std::atomic<bool> post_reset_draw_pending_{false};
    std::atomic<bool> first_end_scene_seen_{false};
};
}
