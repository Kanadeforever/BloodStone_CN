#include "resource_mode.h"
#include "font_atlas.h"
#include "log.h"
namespace npcsub {
namespace {
uint32_t U32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
std::wstring Lower(std::wstring v){for(auto& c:v)if(c>=L'A'&&c<=L'Z')c=wchar_t(c-L'A'+L'a');return v;}
}
bool ValidateBli3(const std::wstring& path) {
    HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr); if(f==INVALID_HANDLE_VALUE) return false;
    uint8_t h[32]{}; DWORD got=0; const bool ok=ReadFile(f,h,sizeof(h),&got,nullptr)!=FALSE&&got==sizeof(h); CloseHandle(f); if(!ok) return false;
    static const uint8_t magic[8]={'B','S','C','N','L','I','3',0};
    return memcmp(h,magic,8)==0 && U32(h+8)==3u && U32(h+12)==32u;
}
const char* ResourceModeName(ResourceMode m){return m==ResourceMode::ChineseBscn?"CHINESE_BSCN":"ENGLISH_NATIVE_FALLBACK";}
ResourceDecision DecideResources(const std::wstring& dir,const Config& c){
    ResourceDecision d{}; const std::wstring root=JoinPath(dir,c.bscn_root); d.bfa_path=JoinPath(root,L"fonts.bfa"); d.bli_path=JoinPath(root,L"loc_context.bli"); d.loc_root=JoinPath(root,L"loc");
    d.bfa_present=FileExists(d.bfa_path); d.bli_present=FileExists(d.bli_path); d.loc_present=DirectoryExists(d.loc_root); d.bli_valid=d.bli_present&&ValidateBli3(d.bli_path);
    if(d.bfa_present){ FontAtlas probe; d.bfa_valid=probe.Load(d.bfa_path); probe.Shutdown(); }
    const std::wstring requested=Lower(c.resource_mode);
    const bool chinese_ready=d.bfa_valid&&d.bli_valid&&d.loc_present;
    if(requested==L"chinese"||requested==L"bscn") d.mode=chinese_ready?ResourceMode::ChineseBscn:ResourceMode::EnglishNative;
    else if(requested==L"english"||requested==L"native") d.mode=ResourceMode::EnglishNative;
    else d.mode=chinese_ready?ResourceMode::ChineseBscn:ResourceMode::EnglishNative;
    NPC_LOG_INFO("RESOURCE_MODE requested=%s selected=%s bfa=%u/%u bli=%u/%u loc=%u",
        WideToUtf8(c.resource_mode).c_str(),ResourceModeName(d.mode),d.bfa_present,d.bfa_valid,d.bli_present,d.bli_valid,d.loc_present);
    return d;
}
}
