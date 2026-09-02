#pragma once
#include "common.h"
#include "config.h"

namespace npcsub {
class DialogueIndex {
public:
    enum class Category : uint8_t {
        Civilian,
        Background,
        Staff,
        Guard,
        RadioLike,
        Other,
    };
    struct Record {
        uint32_t hash = 0;
        std::string key_name;
        std::wstring source_path;
        std::wstring text;
        Category category = Category::Other;
        bool policy_allowed = false;
        bool hard_excluded = false;
    };

    bool Initialize(const std::wstring& module_directory, const Config& config);
    void Shutdown();
    const Record* Find(uint32_t hash) const;
    size_t size() const { return records_.size(); }
    static const char* CategoryName(Category category);

private:
    void CollectLocFiles(const std::wstring& directory, unsigned depth,
                         std::vector<std::wstring>* files) const;
    bool LoadScriptDialogueLoc(const std::wstring& path, const Config& config,
                               size_t* parsed_entries, size_t* accepted_entries);
    static bool IsScriptDialoguePath(const std::wstring& path);
    static Category ClassifyCategory(const std::string& key);
    static bool IsHardExcludedKey(const std::string& key);
    static bool IsAllowedByPolicy(Category category, const Config& config);

    std::vector<Record> records_;
    std::unordered_map<uint32_t, size_t> by_hash_;
    std::unordered_set<uint32_t> ambiguous_hashes_;
};
}
