#include "hooks.h"
#include "external_loc.h"
#include "font_atlas.h"
#include "interop_host.h"
#include "loc_text_bridge.h"
#include "log.h"
#include "native_font_bridge.h"
#include "submit_text_bridge.h"

namespace bscn::hooks {
namespace {
using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);
using CreateDeviceFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3D9*,UINT,D3DDEVTYPE,HWND,DWORD,D3DPRESENT_PARAMETERS*,IDirect3DDevice9**);
using ResetFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*,D3DPRESENT_PARAMETERS*);
using EndSceneFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*);

HMODULE g_main=nullptr;
ExternalFontAtlas* g_atlas=nullptr;
NativeFontBridge* g_native=nullptr;
ExternalLocOverrides* g_overrides=nullptr;
LocTextBridge* g_loc=nullptr;
SubmitTextBridge* g_submit=nullptr;
bool g_gate_text_until_font_ready=false;
Direct3DCreate9Fn g_orig_d3d9=nullptr;
CreateDeviceFn g_orig_create_device=nullptr;
ResetFn g_orig_reset=nullptr;
EndSceneFn g_orig_end_scene=nullptr;
void** g_d3d_create_slot=nullptr;
void** g_dev_reset_slot=nullptr;
void** g_dev_end_scene_slot=nullptr;
std::atomic<DWORD> g_last_maintenance{0};
std::atomic<bool> g_font_render_ready{false};

void PublishFontReady(bool requested_ready) {
    const bool ready = !g_gate_text_until_font_ready ||
        (requested_ready && g_atlas && g_atlas->texture() &&
         g_native && g_native->installed());
    g_font_render_ready.store(ready, std::memory_order_release);
    // W1.1 的 D3D Hook 会早于 ExternalLoc 初始化。此时 overrides 对象存在，
    // 但其索引可能尚未完成；SetRenderingReady 仅写 atomic gate，因此安全。
    // ExternalLoc 初始化结束后 plugin 会按 FontRenderReady() 再同步一次。
    if (g_overrides) g_overrides->SetRenderingReady(ready);
}

void AttachDeviceResources(IDirect3DDevice9* device) {
    if (!device) return;
    if (g_atlas && g_atlas->CreateDeviceResources(device)) {
        if (g_native) g_native->SetTexture(g_atlas->texture());
        PublishFontReady(g_atlas->texture()!=nullptr);
    }
}

void BeforeDeviceReset(IDirect3DDevice9*, const D3DPRESENT_PARAMETERS*) {
    PublishFontReady(false);
    if (g_native) g_native->SetTexture(nullptr);
    if (g_atlas) g_atlas->OnBeforeReset();
}

void AfterDeviceReset(IDirect3DDevice9* device, HRESULT hr) {
    if (SUCCEEDED(hr) && g_atlas && g_atlas->OnAfterReset(device)) {
        if (g_native) g_native->SetTexture(g_atlas->texture());
        PublishFontReady(g_atlas->texture()!=nullptr);
    }
}

HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* device,
                                    D3DPRESENT_PARAMETERS* pp) {
    // BloodstoneCN/NPC release default-pool resources before the frozen
    // BondGamepadIcons 1.4.20 hook (if present) runs. Its managed atlas is not
    // touched by either callback.
    BeforeDeviceReset(device, pp);
    interop::NotifyBeforeReset(device, pp);
    const HRESULT hr=g_orig_reset ? g_orig_reset(device,pp) : D3DERR_INVALIDCALL;
    AfterDeviceReset(device, hr);
    interop::NotifyAfterReset(device, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookEndScene(IDirect3DDevice9* device) {
    // Preserve existing hook order first, then render external clients. This
    // mirrors NPCSubtitles 0.2.2's proven "previous EndScene -> Draw" order.
    const HRESULT hr=g_orig_end_scene ? g_orig_end_scene(device) : D3D_OK;
    interop::NotifyEndScene(device);
    return hr;
}

void HookDeviceVtable(IDirect3DDevice9* device) {
    if (!device) return;
    void*** object=reinterpret_cast<void***>(device);
    void** vt=*object;
    if (!vt) return;

    if (!g_orig_reset) {
        g_dev_reset_slot=&vt[16];
        void* observed=*g_dev_reset_slot;
        if (PatchPointer(g_dev_reset_slot,observed,reinterpret_cast<void*>(&HookReset))) {
            g_orig_reset=reinterpret_cast<ResetFn>(observed);
            BSCN_LOG_INFO("D3D9 Reset hook installed chain_previous=%p", observed);
        }
    }
    if (!g_orig_end_scene) {
        g_dev_end_scene_slot=&vt[42];
        void* observed=*g_dev_end_scene_slot;
        if (PatchPointer(g_dev_end_scene_slot,observed,reinterpret_cast<void*>(&HookEndScene))) {
            g_orig_end_scene=reinterpret_cast<EndSceneFn>(observed);
            BSCN_LOG_INFO("D3D9 EndScene interop host hook installed chain_previous=%p", observed);
        }
    }

    AttachDeviceResources(device);
    interop::NotifyDeviceCreated(device);
}

HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9* self,UINT adapter,
    D3DDEVTYPE type,HWND window,DWORD behavior,D3DPRESENT_PARAMETERS* pp,
    IDirect3DDevice9** out) {
    const HRESULT hr=g_orig_create_device
        ? g_orig_create_device(self,adapter,type,window,behavior,pp,out)
        : D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr) && out && *out) HookDeviceVtable(*out);
    return hr;
}

IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdk) {
    IDirect3D9* d3d=g_orig_d3d9 ? g_orig_d3d9(sdk) : nullptr;
    if (!d3d) return nullptr;
    void** vt=*reinterpret_cast<void***>(d3d);
    if (vt && !g_orig_create_device) {
        g_d3d_create_slot=&vt[16];
        void* observed=*g_d3d_create_slot;
        if (PatchPointer(g_d3d_create_slot,observed,
                         reinterpret_cast<void*>(&HookCreateDevice))) {
            g_orig_create_device=reinterpret_cast<CreateDeviceFn>(observed);
            BSCN_LOG_INFO("D3D9 CreateDevice hook installed chain_previous=%p", observed);
        }
    }
    return d3d;
}

bool FileExists(const std::wstring& path) {
    const DWORD attr=GetFileAttributesW(path.c_str());
    return attr!=INVALID_FILE_ATTRIBUTES && !(attr&FILE_ATTRIBUTE_DIRECTORY);
}

// BondGamepadIcons 1.4.20 is the frozen compatibility anchor. We never
// load, call, patch, or wait for it. The only safe rule for independent ASIs
// is to chain whatever function is currently present in Bond's IAT/vtable.
// This makes load order irrelevant: if GamepadIcons is already installed, CN
// chains on top; if it installs later, its own 1.4.20 PatchPointer chains on
// top of CN. A blocking wait here would risk missing Bond's early CreateDevice.
void ObserveFrozenGamepadAnchor() {
    HMODULE self=nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&g_main),
                       &self);
    const std::wstring dir=ModuleDirectory(self);
    const std::wstring expected=JoinPath(dir,L"BondGamepadIcons.asi");
    if (!FileExists(expected)) {
        BSCN_LOG_INFO("D3D9_ANCHOR_ABSENT module=BondGamepadIcons.asi policy=CHAIN_CURRENT_IAT_NO_WAIT");
        return;
    }
    const bool loaded=GetModuleHandleW(L"BondGamepadIcons.asi")!=nullptr;
    BSCN_LOG_INFO(
        "D3D9_ANCHOR_PRESENT module=BondGamepadIcons.asi version_expected=1.4.20 loaded=%u policy=FROZEN_UNMODIFIED_CHAIN_CURRENT_IAT_NO_WAIT",
        loaded?1u:0u);
}
} // namespace

bool Install(HMODULE main_module,ExternalFontAtlas* atlas,
             NativeFontBridge* native_font,ExternalLocOverrides* overrides,
             LocTextBridge* loc,SubmitTextBridge* submit,
             bool gate_text_until_font_ready) {
    if (!main_module || !atlas || !native_font || !overrides || !loc || !submit)
        return false;
    g_main=main_module;
    g_atlas=atlas;
    g_native=native_font;
    g_overrides=overrides;
    g_loc=loc;
    g_submit=submit;
    g_gate_text_until_font_ready=gate_text_until_font_ready;

    ObserveFrozenGamepadAnchor();
    void* original=nullptr;
    if (!PatchImport(main_module,"d3d9.dll","Direct3DCreate9",
                     reinterpret_cast<void*>(&HookDirect3DCreate9),&original)) {
        BSCN_LOG_ERROR("Direct3DCreate9 IAT hook failed");
        return false;
    }
    g_orig_d3d9=reinterpret_cast<Direct3DCreate9Fn>(original);
    interop::SetHostReady(true);
    BSCN_LOG_INFO("Direct3DCreate9 IAT hook installed mode=EARLY_R10_4_W1_1 interop_host=READY chain_previous=%p atomic_chain=1", original);
    return true;
}

bool FontRenderReady() {
    return g_font_render_ready.load(std::memory_order_acquire);
}

void MaintenanceTick() {
    const DWORD now=GetTickCount();
    DWORD old=g_last_maintenance.load();
    if (old && now-old<100) return;
    if (!g_last_maintenance.compare_exchange_strong(old,now) && now-old<100)
        return;
    if (g_native) g_native->TryInstall();
    PublishFontReady(true);
    // 中文文本 Hook 只有在 NativeGlyph + GPU atlas 真正可渲染后才安装。
    // 这不是削减中文覆盖，而是防止 SubmitText 在字体未就绪时提前输出 CJK，
    // 造成原生字体将汉字画成口口口。
    if (g_gate_text_until_font_ready && !FontRenderReady()) return;
    if (g_loc) g_loc->TryInstall();
    if (g_submit) g_submit->TryInstall();
}

void Shutdown() {
    interop::SetHostReady(false);
    if (g_submit) g_submit->Shutdown();
    if (g_loc) g_loc->Shutdown();
    if (g_native) g_native->Shutdown();
    if (g_dev_end_scene_slot && g_orig_end_scene)
        RestorePointer(g_dev_end_scene_slot,reinterpret_cast<void*>(&HookEndScene),
                       reinterpret_cast<void*>(g_orig_end_scene));
    if (g_dev_reset_slot && g_orig_reset)
        RestorePointer(g_dev_reset_slot,reinterpret_cast<void*>(&HookReset),
                       reinterpret_cast<void*>(g_orig_reset));
    if (g_d3d_create_slot && g_orig_create_device)
        RestorePointer(g_d3d_create_slot,reinterpret_cast<void*>(&HookCreateDevice),
                       reinterpret_cast<void*>(g_orig_create_device));
    interop::Shutdown();
    g_main=nullptr;
    g_atlas=nullptr;
    g_native=nullptr;
    g_overrides=nullptr;
    g_loc=nullptr;
    g_submit=nullptr;
    g_gate_text_until_font_ready=false;
    g_orig_d3d9=nullptr;
    g_orig_create_device=nullptr;
    g_orig_reset=nullptr;
    g_orig_end_scene=nullptr;
    g_d3d_create_slot=nullptr;
    g_dev_reset_slot=nullptr;
    g_dev_end_scene_slot=nullptr;
    g_font_render_ready.store(false, std::memory_order_release);
}
} // namespace bscn::hooks
