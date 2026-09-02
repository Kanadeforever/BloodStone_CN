#include "renderer.h"
#include "log.h"
namespace npcsub {
namespace {
SubtitleRenderer* g_renderer=nullptr;
struct Vertex { float x,y,z,rhw; D3DCOLOR color; float u,v; };
constexpr DWORD kFvf=D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1;
void AddQuad(std::vector<Vertex>* v,float x,float y,float w,float h,float u0,float v0,float u1,float v1,D3DCOLOR c){
    const float x0=x-0.5f,y0=y-0.5f,x1=x+w-0.5f,y1=y+h-0.5f;
    Vertex a{x0,y0,0,1,c,u0,v0},b{x1,y0,0,1,c,u1,v0},d{x0,y1,0,1,c,u0,v1},e{x1,y1,0,1,c,u1,v1};
    v->push_back(a);v->push_back(b);v->push_back(d);v->push_back(d);v->push_back(b);v->push_back(e);
}
void SetupDrawState(IDirect3DDevice9* d,IDirect3DTexture9* texture){
    d->SetVertexShader(nullptr);d->SetPixelShader(nullptr);d->SetVertexDeclaration(nullptr);d->SetFVF(kFvf);d->SetTexture(0,texture);
    d->SetRenderState(D3DRS_ZENABLE,FALSE);d->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);d->SetRenderState(D3DRS_LIGHTING,FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);d->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);d->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);d->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_MODULATE);d->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);d->SetTextureStageState(0,D3DTSS_COLORARG2,D3DTA_DIFFUSE);
    d->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_MODULATE);d->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);d->SetTextureStageState(0,D3DTSS_ALPHAARG2,D3DTA_DIFFUSE);d->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE);d->SetTextureStageState(1,D3DTSS_ALPHAOP,D3DTOP_DISABLE);
    d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);d->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE);d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);d->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
}
void DrawVertices(IDirect3DDevice9* d,IDirect3DTexture9* t,const std::vector<Vertex>& v){if(v.empty()||!t)return;IDirect3DStateBlock9* sb=nullptr;if(FAILED(d->CreateStateBlock(D3DSBT_ALL,&sb))||!sb)return;sb->Capture();SetupDrawState(d,t);d->DrawPrimitiveUP(D3DPT_TRIANGLELIST,UINT(v.size()/3),v.data(),sizeof(Vertex));sb->Apply();sb->Release();}
std::vector<char32_t> ToCodepoints(const std::wstring& s){std::vector<char32_t> out;out.reserve(s.size());for(size_t i=0;i<s.size();++i){uint32_t u=uint16_t(s[i]);if(u>=0xD800&&u<=0xDBFF&&i+1<s.size()){uint32_t lo=uint16_t(s[i+1]);if(lo>=0xDC00&&lo<=0xDFFF){out.push_back(char32_t(0x10000+((u-0xD800)<<10)+(lo-0xDC00)));++i;continue;}}out.push_back(char32_t(u));}return out;}
}

bool SubtitleRenderer::Prepare(HMODULE main,const Config& c,ResourceMode mode,FontAtlas* atlas,NativeFontCapture* native_font){Shutdown();if(!main||g_renderer)return false;main_=main;config_=c;mode_=mode;atlas_=atlas;native_font_=native_font;g_renderer=this;return true;}
bool SubtitleRenderer::Install(){if(installed_.load())return true;if(!main_)return false;void* prev=nullptr;if(!PatchImportChain(main_,"d3d9.dll","Direct3DCreate9",reinterpret_cast<void*>(&HookDirect3DCreate9),&prev)){NPC_LOG_ERROR("D3D9_IAT_HOOK_FAILED");return false;}previous_d3d9_=reinterpret_cast<Direct3DCreate9Fn>(prev);installed_.store(true);NPC_LOG_INFO("NPC_SUBTITLE_D3D9_IAT_HOOK_OK mode=%s",ResourceModeName(mode_));if(!BootstrapDeviceHooks())NPC_LOG_WARN("NPC_SUBTITLE_D3D9_BOOTSTRAP_FAILED action=KEEP_IAT_FUTURE_CREATEDEVICE_PATH");return true;}
bool SubtitleRenderer::HookD3D9(IDirect3D9* d,const char* source){if(!d)return false;void** vt=*reinterpret_cast<void***>(d);if(!vt)return false;if(!previous_create_){create_slot_=&vt[16];void* prev=nullptr;if(!PatchPointerChain(create_slot_,reinterpret_cast<void*>(&HookCreateDevice),&prev))return false;previous_create_=reinterpret_cast<CreateDeviceFn>(prev);NPC_LOG_INFO("NPC_SUBTITLE_CREATE_DEVICE_HOOK_OK source=%s",source?source:"unknown");}return true;}
IDirect3D9* WINAPI SubtitleRenderer::HookDirect3DCreate9(UINT sdk){SubtitleRenderer* r=g_renderer;IDirect3D9* d=r&&r->previous_d3d9_?r->previous_d3d9_(sdk):nullptr;if(r&&d)r->HookD3D9(d,"iat");return d;}
HRESULT STDMETHODCALLTYPE SubtitleRenderer::HookCreateDevice(IDirect3D9* self,UINT a,D3DDEVTYPE t,HWND w,DWORD b,D3DPRESENT_PARAMETERS* pp,IDirect3DDevice9** out){SubtitleRenderer* r=g_renderer;HRESULT hr=r&&r->previous_create_?r->previous_create_(self,a,t,w,b,pp,out):D3DERR_INVALIDCALL;if(SUCCEEDED(hr)&&r&&out&&*out)r->HookDevice(*out,true);return hr;}
bool SubtitleRenderer::BootstrapDeviceHooks(){
    NPC_LOG_INFO("NPC_SUBTITLE_D3D9_BOOTSTRAP_BEGIN reason=RECOVER_ALREADY_CREATED_DEVICE mode=INTEROP_SAFE_RAW_EXPORT");
    // 0.2.1 used previous_d3d9_/previous_create_.  In a multi-ASI process those
    // pointers may already be another plugin's chain hooks.  Creating our dummy
    // device through that chain can make the other plugin mistake the 8x8 dummy
    // device for Bond's real device and initialize its GPU resources on it.
    // Bootstrap must therefore bypass the executable IAT chain completely and
    // must never patch/call a foreign IDirect3D9::CreateDevice hook.
    HMODULE d3d_module=GetModuleHandleW(L"d3d9.dll");
    if(!d3d_module){NPC_LOG_WARN("NPC_SUBTITLE_D3D9_BOOTSTRAP_NO_D3D9_MODULE action=IAT_ONLY");return false;}
    auto raw_d3d9=reinterpret_cast<Direct3DCreate9Fn>(GetProcAddress(d3d_module,"Direct3DCreate9"));
    if(!raw_d3d9){NPC_LOG_WARN("NPC_SUBTITLE_D3D9_BOOTSTRAP_NO_RAW_EXPORT action=IAT_ONLY");return false;}
    IDirect3D9* d3d=raw_d3d9(D3D_SDK_VERSION);
    if(!d3d){NPC_LOG_WARN("NPC_SUBTITLE_D3D9_BOOTSTRAP_NO_D3D9 action=IAT_ONLY");return false;}
    void** d3d_vt=*reinterpret_cast<void***>(d3d);
    CreateDeviceFn raw_create=d3d_vt?reinterpret_cast<CreateDeviceFn>(d3d_vt[16]):nullptr;
    HMODULE create_owner=nullptr;
    if(raw_create){
        MEMORY_BASIC_INFORMATION mbi{};
        const uintptr_t create_address=reinterpret_cast<uintptr_t>(raw_create);
        if(VirtualQuery(reinterpret_cast<LPCVOID>(create_address),&mbi,sizeof(mbi))==sizeof(mbi))
            create_owner=static_cast<HMODULE>(mbi.AllocationBase);
    }
    if(!raw_create||create_owner!=d3d_module){
        wchar_t owner_path[512]{};
        if(create_owner)GetModuleFileNameW(create_owner,owner_path,DWORD(std::size(owner_path)));
        NPC_LOG_WARN("NPC_SUBTITLE_D3D9_BOOTSTRAP_SKIP_FOREIGN_CREATEDEVICE owner=%ls action=IAT_ONLY no_dummy_device=1",
                     owner_path[0]?owner_path:L"<unknown>");
        d3d->Release();
        return false;
    }
    NPC_LOG_INFO("NPC_SUBTITLE_D3D9_BOOTSTRAP_CREATEDEVICE_CLEAN owner=d3d9.dll chain_bypass=1");
    static const wchar_t kClassName[]=L"NPCSubtitles_D3D9Bootstrap_Window";WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=kClassName;ATOM atom=RegisterClassW(&wc);if(!atom&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS){d3d->Release();NPC_LOG_WARN("NPC_SUBTITLE_D3D9_BOOTSTRAP_WINDOW_CLASS_FAILED err=%lu",GetLastError());return false;}HWND hwnd=CreateWindowExW(0,kClassName,L"",WS_OVERLAPPED,0,0,8,8,nullptr,nullptr,wc.hInstance,nullptr);if(!hwnd){d3d->Release();NPC_LOG_WARN("NPC_SUBTITLE_D3D9_BOOTSTRAP_WINDOW_FAILED err=%lu",GetLastError());return false;}
    D3DPRESENT_PARAMETERS pp{};pp.BackBufferWidth=8;pp.BackBufferHeight=8;pp.BackBufferFormat=D3DFMT_UNKNOWN;pp.BackBufferCount=1;pp.MultiSampleType=D3DMULTISAMPLE_NONE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=hwnd;pp.Windowed=TRUE;pp.EnableAutoDepthStencil=FALSE;pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;IDirect3DDevice9* dev=nullptr;const DWORD behavior=D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE|D3DCREATE_NOWINDOWCHANGES;HRESULT hr=raw_create(d3d,D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,hwnd,behavior,&pp,&dev);bool ok=false;if(SUCCEEDED(hr)&&dev){ok=HookDevice(dev,false);dev->Release();}DestroyWindow(hwnd);d3d->Release();if(ok)NPC_LOG_INFO("NPC_SUBTITLE_D3D9_BOOTSTRAP_OK device_type=HAL reset=%u endscene=%u create_hook_side_effect=0",previous_reset_?1u:0u,previous_end_scene_?1u:0u);else NPC_LOG_WARN("NPC_SUBTITLE_D3D9_BOOTSTRAP_DEVICE_FAILED hr=0x%08X",unsigned(hr));return ok;}
bool SubtitleRenderer::HookDevice(IDirect3DDevice9* d,bool initialize_resources){if(!d)return false;void** vt=*reinterpret_cast<void***>(d);if(!previous_reset_){reset_slot_=&vt[16];void* prev=nullptr;if(!PatchPointerChain(reset_slot_,reinterpret_cast<void*>(&HookReset),&prev))return false;previous_reset_=reinterpret_cast<ResetFn>(prev);NPC_LOG_INFO("NPC_SUBTITLE_RESET_HOOK_OK");}if(!previous_end_scene_){end_scene_slot_=&vt[42];void* prev=nullptr;if(!PatchPointerChain(end_scene_slot_,reinterpret_cast<void*>(&HookEndScene),&prev))return false;previous_end_scene_=reinterpret_cast<EndSceneFn>(prev);NPC_LOG_INFO("NPC_SUBTITLE_ENDSCENE_HOOK_OK");}if(initialize_resources&&mode_==ResourceMode::ChineseBscn&&atlas_){atlas_->InvalidateGpuTexture();atlas_->EnsureTexture(d);}return true;}
HRESULT STDMETHODCALLTYPE SubtitleRenderer::HookEndScene(IDirect3DDevice9* d){SubtitleRenderer* r=g_renderer;HRESULT hr=r&&r->previous_end_scene_?r->previous_end_scene_(d):D3D_OK;if(r){if(!r->first_end_scene_seen_.exchange(true)){D3DVIEWPORT9 vp{};const bool ok=SUCCEEDED(d->GetViewport(&vp));NPC_LOG_INFO("NPC_SUBTITLE_ENDSCENE_ACTIVE viewport=%ux%u",ok?unsigned(vp.Width):0u,ok?unsigned(vp.Height):0u);}r->Draw(d);}return hr;}
HRESULT STDMETHODCALLTYPE SubtitleRenderer::HookReset(IDirect3DDevice9* d,D3DPRESENT_PARAMETERS* pp){SubtitleRenderer* r=g_renderer;if(!r||!r->previous_reset_)return D3DERR_INVALIDCALL;r->BeforeReset(d,pp);HRESULT hr=r->previous_reset_(d,pp);r->AfterReset(d,hr);return hr;}
void SubtitleRenderer::BeforeReset(IDirect3DDevice9*,const D3DPRESENT_PARAMETERS* pp){if(mode_==ResourceMode::ChineseBscn&&atlas_)atlas_->InvalidateGpuTexture();if(mode_==ResourceMode::EnglishNative&&native_font_)native_font_->InvalidateGpuResources();if(pp)NPC_LOG_INFO("NPC_SUBTITLE_DEVICE_RESET_BEGIN request=%ux%u windowed=%u",unsigned(pp->BackBufferWidth),unsigned(pp->BackBufferHeight),pp->Windowed?1u:0u);else NPC_LOG_INFO("NPC_SUBTITLE_DEVICE_RESET_BEGIN request=unknown");}
void SubtitleRenderer::AfterReset(IDirect3DDevice9* d,HRESULT hr){if(FAILED(hr)){NPC_LOG_WARN("NPC_SUBTITLE_DEVICE_RESET_FAILED hr=0x%08X",unsigned(hr));return;}int chinese_texture=-1;if(mode_==ResourceMode::ChineseBscn&&atlas_)chinese_texture=atlas_->EnsureTexture(d)?1:0;D3DVIEWPORT9 vp{};const bool vp_ok=SUCCEEDED(d->GetViewport(&vp));post_reset_draw_pending_.store(true);NPC_LOG_INFO("NPC_SUBTITLE_DEVICE_RESET_OK mode=%s chinese_texture=%d viewport=%ux%u",ResourceModeName(mode_),chinese_texture,vp_ok?unsigned(vp.Width):0u,vp_ok?unsigned(vp.Height):0u);}
void SubtitleRenderer::Enqueue(uint32_t hash,const std::wstring& text){if(text.empty())return;DWORD now=GetTickCount();DWORD dur=config_.min_duration_ms+DWORD(std::min<size_t>(text.size(),500))*config_.duration_per_char_ms;dur=std::clamp(dur,config_.min_duration_ms,config_.max_duration_ms);std::lock_guard<std::mutex> lock(slots_mutex_);for(auto& s:slots_){if(s.hash==hash&&s.expire>now&&now-s.start<config_.dedupe_ms){s.expire=now+dur;return;}}Slot* chosen=nullptr;for(auto& s:slots_)if(s.expire<=now){chosen=&s;break;}if(!chosen)chosen=&*std::min_element(slots_.begin(),slots_.end(),[](const Slot&a,const Slot&b){return a.start<b.start;});chosen->hash=hash;chosen->start=now;chosen->expire=now+dur;chosen->text=text;NPC_LOG_INFO("NPC_SUBTITLE_QUEUE hash=0x%08X units=%u duration=%lu mode=%s",hash,unsigned(text.size()),dur,ResourceModeName(mode_));}
void SubtitleRenderer::Draw(IDirect3DDevice9* d){if(!d)return;if(post_reset_draw_pending_.exchange(false)){D3DVIEWPORT9 post{};const bool ok=SUCCEEDED(d->GetViewport(&post));NPC_LOG_INFO("NPC_SUBTITLE_POST_RESET_DRAW_OK viewport=%ux%u",ok?unsigned(post.Width):0u,ok?unsigned(post.Height):0u);}DWORD now=GetTickCount();std::vector<Slot> active;{std::lock_guard<std::mutex> lock(slots_mutex_);for(const auto&s:slots_)if(s.expire>now&&!s.text.empty())active.push_back(s);}if(active.empty())return;std::sort(active.begin(),active.end(),[](const Slot&a,const Slot&b){return a.start<b.start;});if(active.size()>size_t(config_.max_visible_lines))active.erase(active.begin(),active.end()-config_.max_visible_lines);D3DVIEWPORT9 vp{};if(FAILED(d->GetViewport(&vp))||!vp.Height)return;float y=float(vp.Height)*config_.base_y;const float block=float(vp.Height)/720.0f*(mode_==ResourceMode::ChineseBscn?32.0f:26.0f);for(const auto&s:active){if(mode_==ResourceMode::ChineseBscn)DrawChinese(d,s.text,y);else DrawEnglish(d,s.text,y);y+=block*1.30f;}}

void SubtitleRenderer::DrawChinese(IDirect3DDevice9* d,const std::wstring& text,float y){if(!atlas_||!atlas_->loaded()||!atlas_->EnsureTexture(d))return;D3DVIEWPORT9 vp{};if(FAILED(d->GetViewport(&vp)))return;const float scale=float(vp.Height)/720.0f*config_.scale,maxw=float(vp.Width)*config_.max_width;auto cps=ToCodepoints(text);if(cps.empty())return;
    struct Line{size_t a,b;float w,h;};std::vector<Line> lines;size_t begin=0;float w=0,h=0;for(size_t i=0;i<cps.size();++i){const GlyphInfo*g=atlas_->Find(cps[i]);float adv=(g?g->advance_x:16)*scale;if(w>0&&w+adv>maxw&&lines.size()<2){lines.push_back({begin,i,w,h});begin=i;w=0;h=0;}w+=adv;h=std::max(h,float(g?g->height:16)*scale);}lines.push_back({begin,cps.size(),w,h});
    std::vector<float> tops(lines.size(),y);const float px=float(vp.Height)/720.0f;for(size_t li=1;li<lines.size();++li){const float nominal=atlas_->line_height()*scale*config_.line_spacing;const float minimum=lines[li-1].h+1.0f*px;tops[li]=tops[li-1]+std::max(nominal,minimum);}
    std::vector<Vertex> shadow,main;for(size_t li=0;li<lines.size();++li){float pen=float(vp.X)+(float(vp.Width)-lines[li].w)*0.5f;float top=tops[li];for(size_t i=lines[li].a;i<lines[li].b;++i){const GlyphInfo*g=atlas_->Find(cps[i]);if(!g){pen+=16*scale;continue;}float gx=pen+g->bearing_x*scale,gy=top+std::max(0.0f,(atlas_->line_height()-g->height)*0.5f*scale);float gw=g->width*scale,gh=g->height*scale;AddQuad(&shadow,gx+1.35f,gy+1.35f,gw,gh,g->u0,g->v0,g->u1,g->v1,0xA8000000);AddQuad(&main,gx,gy,gw,gh,g->u0,g->v0,g->u1,g->v1,0xEFFFFFFF);pen+=g->advance_x*scale;}}
    DrawVertices(d,atlas_->texture(),shadow);DrawVertices(d,atlas_->texture(),main);
}

void SubtitleRenderer::DrawEnglish(IDirect3DDevice9* d,const std::wstring& text,float y){if(!native_font_)return;NativeRenderContext c=native_font_->SnapshotContext();if(!c.valid()){static DWORD last=0;DWORD now=GetTickCount();if(!last||now-last>2000){last=now;NPC_LOG_INFO("NPC_NATIVE_FONT_WAIT");}return;}D3DVIEWPORT9 vp{};if(FAILED(d->GetViewport(&vp)))return;const float factor=float(vp.Height)/720.0f*(config_.scale/0.56f),maxw=float(vp.Width)*config_.max_width;
    struct M{NativeRenderGlyph g;wchar_t ch;};std::vector<M> metrics;metrics.reserve(text.size());for(wchar_t ch:text){NativeRenderGlyph g{};if(ch==L' '){if(!native_font_->LookupGlyph(c,L'A',&g)){g.advance=8;g.height=18;}g.advance*=0.45f;g.width=0;}else if(!native_font_->LookupGlyph(c,ch,&g)){g.advance=8;g.width=0;g.height=18;}metrics.push_back({g,ch});}
    struct Line{size_t a,b;float w,h;};std::vector<Line> lines;size_t begin=0;float width=0,height=0;for(size_t i=0;i<metrics.size();++i){float adv=metrics[i].g.advance*factor;if(width>0&&width+adv>maxw&&lines.size()<2){lines.push_back({begin,i,width,height});begin=i;width=0;height=0;}width+=adv;height=std::max(height,metrics[i].g.height*factor);}lines.push_back({begin,metrics.size(),width,height});
    std::vector<float> tops(lines.size(),y);const float px=float(vp.Height)/720.0f;for(size_t li=1;li<lines.size();++li){const float nominal=std::max(18.0f*factor,lines[li-1].h)*config_.line_spacing;const float minimum=lines[li-1].h+1.0f*px;tops[li]=tops[li-1]+std::max(nominal,minimum);}
    std::vector<Vertex> shadow,main;for(size_t li=0;li<lines.size();++li){float pen=float(vp.X)+(float(vp.Width)-lines[li].w)*0.5f;float top=tops[li];for(size_t i=lines[li].a;i<lines[li].b;++i){const auto&m=metrics[i];if(m.ch!=L' '&&m.g.width>0){float gw=m.g.width*factor,gh=m.g.height*factor,gy=top+std::max(0.0f,(lines[li].h-gh)*0.5f);AddQuad(&shadow,pen+1.0f,gy+1.0f,gw,gh,m.g.u0,m.g.v0,m.g.u1,m.g.v1,0xA8000000);AddQuad(&main,pen,gy,gw,gh,m.g.u0,m.g.v0,m.g.u1,m.g.v1,0xEFFFFFFF);}pen+=m.g.advance*factor;}}
    DrawVertices(d,c.texture,shadow);DrawVertices(d,c.texture,main);
}
void SubtitleRenderer::Shutdown(){installed_.store(false);post_reset_draw_pending_.store(false);first_end_scene_seen_.store(false);if(end_scene_slot_&&previous_end_scene_)RestorePointerIfOwned(end_scene_slot_,reinterpret_cast<void*>(&HookEndScene),reinterpret_cast<void*>(previous_end_scene_));if(reset_slot_&&previous_reset_)RestorePointerIfOwned(reset_slot_,reinterpret_cast<void*>(&HookReset),reinterpret_cast<void*>(previous_reset_));if(create_slot_&&previous_create_)RestorePointerIfOwned(create_slot_,reinterpret_cast<void*>(&HookCreateDevice),reinterpret_cast<void*>(previous_create_));{std::lock_guard<std::mutex> lock(slots_mutex_);for(auto&s:slots_)s=Slot{};}main_=nullptr;atlas_=nullptr;native_font_=nullptr;previous_d3d9_=nullptr;previous_create_=nullptr;previous_end_scene_=nullptr;previous_reset_=nullptr;create_slot_=nullptr;end_scene_slot_=nullptr;reset_slot_=nullptr;if(g_renderer==this)g_renderer=nullptr;}
}
