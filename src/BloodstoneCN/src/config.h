#pragma once
#include "common.h"

namespace bscn {
struct Config {
    bool enable = true;
    bool enable_native_glyph = true;
    bool enable_external_loc = true;
    bool enable_text_coverage = true;
    bool require_atlas = true;
    bool require_index = true;
    bool logging = true;
    bool log_coverage_misses = false;
    bool log_action_tokens = false;
    bool hud_bypass_dynamic_scalars = true;
    bool hud_defer_technical_labels = true;
    bool hud_lifecycle_probe = false;
    std::wstring atlas_file = L"BSCNContents\\fonts.bfa";
    std::wstring external_loc_root = L"BSCNContents\\loc";
    std::wstring external_loc_language = L"en";
    std::wstring index_file = L"BSCNContents\\loc_context.bli";
};
Config LoadConfig(const std::wstring& path);
}
