#include "font_atlas.h"
#include "log.h"
namespace npcsub {
namespace {
#pragma pack(push,1)
struct AtlasHeaderDisk {
    char magic[8]; uint32_t version,header_size,width,height,line_height,ascent,glyph_count,glyph_record_size,
    pixels_offset,pixels_size,pixel_format,token_width; uint8_t token_prefix0,token_prefix1,token_alphabet,reserved_byte; uint32_t reserved;
};
struct GlyphRecordDisk {
    uint32_t codepoint,carrier_token; uint16_t x,y,width,height; int16_t bearing_x,bearing_y,advance_x; uint16_t flags; uint32_t reserved0,reserved1;
};
#pragma pack(pop)
static_assert(sizeof(AtlasHeaderDisk)==64);
static_assert(sizeof(GlyphRecordDisk)==32);
constexpr char kMagic[8]={'B','S','C','N','A','T','L','1'};
bool Scalar(uint32_t v) { return v<=0x10FFFFu && !(v>=0xD800u&&v<=0xDFFFu); }
bool ReadWhole(const std::wstring& path,std::vector<uint8_t>* out) {
    out->clear(); HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE) return false; LARGE_INTEGER s{}; bool ok=GetFileSizeEx(f,&s)!=FALSE && s.QuadPart>0 && s.QuadPart<=64ll*1024ll*1024ll;
    if(ok){ out->resize(static_cast<size_t>(s.QuadPart)); size_t pos=0; while(pos<out->size()){ DWORD want=static_cast<DWORD>(std::min<size_t>(out->size()-pos,1u<<20)); DWORD got=0; if(!ReadFile(f,out->data()+pos,want,&got,nullptr)||got!=want){ok=false;break;} pos+=got; } }
    CloseHandle(f); if(!ok) out->clear(); return ok;
}
}
FontAtlas::~FontAtlas(){Shutdown();}
bool FontAtlas::Load(const std::wstring& path) {
    Shutdown(); std::vector<uint8_t> file; if(!ReadWhole(path,&file)||file.size()<64) return false;
    AtlasHeaderDisk h{}; memcpy(&h,file.data(),sizeof(h));
    const uint64_t rec=uint64_t(h.glyph_count)*h.glyph_record_size, expected=uint64_t(h.width)*h.height, end=uint64_t(h.pixels_offset)+h.pixels_size;
    if(memcmp(h.magic,kMagic,8)!=0||h.version!=1||h.header_size!=64||h.glyph_record_size!=32||h.pixel_format!=1||
       h.width==0||h.width>8192||h.height==0||h.height>8192||h.glyph_count==0||h.glyph_count>200000||
       uint64_t(h.header_size)+rec>h.pixels_offset||expected!=h.pixels_size||end>file.size()) return false;
    std::unordered_map<char32_t,GlyphInfo> glyphs; glyphs.reserve(h.glyph_count);
    for(uint32_t i=0;i<h.glyph_count;++i){ GlyphRecordDisk r{}; memcpy(&r,file.data()+h.header_size+size_t(i)*h.glyph_record_size,sizeof(r));
        if(!Scalar(r.codepoint)||uint32_t(r.x)+r.width>h.width||uint32_t(r.y)+r.height>h.height||glyphs.count(char32_t(r.codepoint))) return false;
        GlyphInfo g{}; g.codepoint=char32_t(r.codepoint); g.width=r.width; g.height=r.height; g.bearing_x=r.bearing_x; g.bearing_y=r.bearing_y; g.advance_x=r.advance_x;
        g.u0=float(r.x)/h.width; g.v0=float(r.y)/h.height; g.u1=float(r.x+r.width)/h.width; g.v1=float(r.y+r.height)/h.height; glyphs.emplace(g.codepoint,g);
    }
    width_=int(h.width); height_=int(h.height); line_height_=int(h.line_height); ascent_=int(h.ascent); pixels_.assign(file.begin()+h.pixels_offset,file.begin()+size_t(end)); glyphs_.swap(glyphs); loaded_=true;
    NPC_LOG_INFO("BFA_CPU_OK path=%s glyphs=%u size=%dx%d",WideToUtf8(path).c_str(),unsigned(glyphs_.size()),width_,height_); return true;
}
bool FontAtlas::EnsureTexture(IDirect3DDevice9* device) { if(!loaded_||!device) return false; if(texture_&&device_==device) return true; InvalidateGpuTexture(); return CreateTexture(device); }
void FontAtlas::InvalidateGpuTexture(){ if(texture_){texture_->Release();texture_=nullptr;} device_=nullptr; }
bool FontAtlas::CreateTexture(IDirect3DDevice9* device) {
    device_=device; HRESULT hr=device->CreateTexture(UINT(width_),UINT(height_),1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture_,nullptr); if(FAILED(hr)||!texture_) return false;
    D3DLOCKED_RECT l{}; if(FAILED(texture_->LockRect(0,&l,nullptr,0))){texture_->Release();texture_=nullptr;return false;}
    for(int y=0;y<height_;++y){ auto* dst=reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(l.pBits)+size_t(y)*l.Pitch); const uint8_t* src=pixels_.data()+size_t(y)*width_; for(int x=0;x<width_;++x){uint32_t a=src[x];dst[x]=(a<<24)|(a<<16)|(a<<8)|a;} }
    texture_->UnlockRect(0); NPC_LOG_INFO("BFA_GPU_OK texture=%p",texture_); return true;
}
void FontAtlas::Shutdown(){ InvalidateGpuTexture(); loaded_=false; width_=height_=0; line_height_=ascent_=1; pixels_.clear(); glyphs_.clear(); }
const GlyphInfo* FontAtlas::Find(char32_t cp) const { auto it=glyphs_.find(cp); return it==glyphs_.end()?nullptr:&it->second; }
}
