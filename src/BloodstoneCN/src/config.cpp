#include "config.h"

namespace bscn {
namespace {
bool ReadBool(const std::wstring& path,const wchar_t* section,
              const wchar_t* key,bool fallback) {
    return GetPrivateProfileIntW(section,key,fallback?1:0,path.c_str())!=0;
}
std::wstring ReadString(const std::wstring& path,const wchar_t* section,
                        const wchar_t* key,const wchar_t* fallback) {
    wchar_t buffer[1024]{};
    GetPrivateProfileStringW(section,key,fallback,buffer,
                             static_cast<DWORD>(sizeof(buffer)/sizeof(buffer[0])),
                             path.c_str());
    return buffer;
}
}

Config LoadConfig(const std::wstring& path) {
    Config c;
    c.enable=ReadBool(path,L"General",L"Enable",true);
    c.enable_native_glyph=ReadBool(path,L"General",L"EnableNativeGlyph",true);
    c.enable_external_loc=ReadBool(path,L"ExternalLoc",L"Enable",true);
    c.enable_text_coverage=ReadBool(path,L"TextCoverage",L"Enable",true);
    c.require_atlas=ReadBool(path,L"General",L"RequireAtlas",true);
    c.require_index=ReadBool(path,L"ExternalLoc",L"RequireIndex",true);
    c.logging=ReadBool(path,L"General",L"Logging",true);
    c.log_coverage_misses=ReadBool(path,L"TextCoverage",L"LogMisses",false);
    c.log_action_tokens=ReadBool(path,L"TextCoverage",L"LogActionTokens",false);
    c.hud_bypass_dynamic_scalars=ReadBool(path,L"HudLifecycle",L"BypassDynamicScalars",true);
    c.hud_defer_technical_labels=ReadBool(path,L"HudLifecycle",L"DeferTechnicalIdentityLabels",true);
    c.hud_lifecycle_probe=ReadBool(path,L"HudLifecycle",L"LifecycleProbe",false);
    c.atlas_file=ReadString(path,L"General",L"AtlasFile",L"BSCNContents\\fonts.bfa");
    c.external_loc_root=ReadString(path,L"ExternalLoc",L"Root",L"BSCNContents\\loc");
    c.external_loc_language=ReadString(path,L"ExternalLoc",L"Language",L"en");
    c.index_file=ReadString(path,L"ExternalLoc",L"Index",L"BSCNContents\\loc_context.bli");
    return c;
}
}
