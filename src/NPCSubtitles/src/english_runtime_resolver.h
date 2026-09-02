#pragma once
#include "common.h"
namespace npcsub {
struct EnglishResolveResult {
    std::wstring text;
    std::string key;
    int score = -1;
    uintptr_t source_address = 0;
    const char* encoding = "NONE";
};
class EnglishRuntimeResolver {
public:
    EnglishResolveResult Resolve(void* result_object, bool log_candidates) const;
    static bool IsHardExcludedKey(const std::string& key);
    static bool LooksLikeDialogueText(const std::wstring& text, int* score = nullptr);
private:
    void ScanRegion(uintptr_t address, size_t bytes, int depth,
                    bool log_candidates, EnglishResolveResult* best) const;
};
}
