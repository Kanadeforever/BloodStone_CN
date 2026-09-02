#pragma once
#include "common.h"
namespace npcsub {
struct Config {
    bool enable = true;
    bool logging = true;
    std::wstring resource_mode = L"Auto";
    std::wstring bscn_root = L"BSCNContents";
    std::wstring loc_language = L"en";
    float scale = 0.56f;
    float base_y = 0.120f;
    float max_width = 0.66f;
    float line_spacing = 0.82f;
    int max_visible_lines = 3;
    DWORD dedupe_ms = 900;
    DWORD min_duration_ms = 2400;
    DWORD max_duration_ms = 6500;
    DWORD duration_per_char_ms = 90;
    bool coverage_first = true;
    bool show_guards = true;
    bool show_radio_like = true;
    bool allow_unknown_english_dialogue = true;
    bool log_english_candidates = true;
    bool battle_subtitles = false;
};
Config LoadConfig(const std::wstring& module_directory);
std::wstring ConfigPath(const std::wstring& module_directory);
}
