#pragma once
#include "common.h"
#include "config.h"
namespace npcsub {
enum class ResourceMode { ChineseBscn, EnglishNative };
struct ResourceDecision {
    ResourceMode mode = ResourceMode::EnglishNative;
    bool bfa_present=false,bfa_valid=false,bli_present=false,bli_valid=false,loc_present=false;
    std::wstring bfa_path,bli_path,loc_root;
};
bool ValidateBli3(const std::wstring& path);
ResourceDecision DecideResources(const std::wstring& module_directory,const Config& config);
const char* ResourceModeName(ResourceMode mode);
}
