#include "config.h"
namespace npcsub {
namespace {
int ReadInt(const std::wstring& path, const wchar_t* section, const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, path.c_str()));
}
float ReadFloat(const std::wstring& path, const wchar_t* section, const wchar_t* key, float fallback) {
    wchar_t buf[64]{};
    wchar_t def[32]{};
    _snwprintf_s(def, std::size(def), _TRUNCATE, L"%.4f", fallback);
    GetPrivateProfileStringW(section, key, def, buf, static_cast<DWORD>(std::size(buf)), path.c_str());
    wchar_t* end = nullptr;
    const double value = wcstod(buf, &end);
    return end == buf ? fallback : static_cast<float>(value);
}
std::wstring ReadString(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* fallback) {
    wchar_t buf[512]{};
    GetPrivateProfileStringW(section, key, fallback, buf, static_cast<DWORD>(std::size(buf)), path.c_str());
    return buf;
}
}
std::wstring ConfigPath(const std::wstring& module_directory) { return JoinPath(module_directory, L"NPCSubtitles.ini"); }
Config LoadConfig(const std::wstring& module_directory) {
    Config c{};
    const std::wstring path = ConfigPath(module_directory);
    c.enable = ReadInt(path,L"General",L"Enable",1)!=0;
    c.logging = ReadInt(path,L"General",L"Logging",1)!=0;
    c.resource_mode = ReadString(path,L"Resources",L"Mode",L"Auto");
    c.bscn_root = ReadString(path,L"Resources",L"BSCNRoot",L"BSCNContents");
    c.loc_language = ReadString(path,L"Resources",L"LocLanguage",L"en");
    c.scale = std::clamp(ReadFloat(path,L"Display",L"Scale",0.56f),0.20f,2.0f);
    c.base_y = std::clamp(ReadFloat(path,L"Display",L"BaseY",0.120f),0.02f,0.90f);
    c.max_width = std::clamp(ReadFloat(path,L"Display",L"MaxWidth",0.66f),0.20f,0.95f);
    c.line_spacing = std::clamp(ReadFloat(path,L"Display",L"LineSpacing",0.82f),0.60f,1.50f);
    c.max_visible_lines = std::clamp(ReadInt(path,L"Display",L"MaxVisibleLines",3),1,6);
    c.dedupe_ms = static_cast<DWORD>(std::clamp(ReadInt(path,L"Timing",L"DedupeMs",900),0,10000));
    c.min_duration_ms = static_cast<DWORD>(std::clamp(ReadInt(path,L"Timing",L"MinDurationMs",2400),500,30000));
    c.max_duration_ms = static_cast<DWORD>(std::clamp(ReadInt(path,L"Timing",L"MaxDurationMs",6500),1000,60000));
    c.duration_per_char_ms = static_cast<DWORD>(std::clamp(ReadInt(path,L"Timing",L"DurationPerCharMs",90),0,1000));
    if (c.max_duration_ms < c.min_duration_ms) c.max_duration_ms = c.min_duration_ms;
    c.coverage_first = ReadInt(path,L"Scope",L"CoverageFirst",1)!=0;
    c.show_guards = ReadInt(path,L"Scope",L"ShowGuards",1)!=0;
    c.show_radio_like = ReadInt(path,L"Scope",L"ShowRadioLike",1)!=0;
    c.battle_subtitles = ReadInt(path,L"Scope",L"BattleSubtitles",0)!=0;
    c.allow_unknown_english_dialogue = ReadInt(path,L"EnglishFallback",L"AllowUnknownDialogue",1)!=0;
    c.log_english_candidates = ReadInt(path,L"Debug",L"LogEnglishCandidates",1)!=0;
    return c;
}
}
