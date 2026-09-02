#include "dialogue_index.h"
#include "log.h"

namespace npcsub {
namespace {
constexpr unsigned kMaximumLocDepth = 32;
constexpr uint32_t kMaximumEntries = 200000u;
constexpr size_t kMaximumFileBytes = 64u * 1024u * 1024u;
constexpr size_t kLanguageDirectory = 0x1Cu;
constexpr size_t kLanguageRecordSize = 24u;
constexpr size_t kLanguageCount = 8u;

uint32_t U32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8u) |
           (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}
uint16_t U16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8u);
}
wchar_t LowerAscii(wchar_t c) {
    return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c-L'A'+L'a') : c;
}
char UpperAscii(char c) {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c-'a'+'A') : c;
}

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>* out) {
    if (!out) return false;
    out->clear();
    HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,
                         OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (f==INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok=GetFileSizeEx(f,&size)!=FALSE && size.QuadPart>=0 &&
            static_cast<uint64_t>(size.QuadPart)<=kMaximumFileBytes;
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        size_t at=0;
        while (at<out->size()) {
            const DWORD want=static_cast<DWORD>(std::min<size_t>(
                out->size()-at,static_cast<size_t>(1u<<20)));
            DWORD got=0;
            if (!ReadFile(f,out->data()+at,want,&got,nullptr) || got!=want) {
                ok=false; break;
            }
            at+=got;
        }
    }
    CloseHandle(f);
    if (!ok) out->clear();
    return ok;
}

bool ReadAsciiZ(const std::vector<uint8_t>& file, size_t start, size_t end,
                std::string* out) {
    if (!out || start>=end || end>file.size()) return false;
    out->clear();
    for (size_t p=start;p<end && out->size()<512u;++p) {
        const uint8_t c=file[p];
        if (!c) return !out->empty();
        if (c<0x20u || c>0x7Eu) return false;
        out->push_back(static_cast<char>(c));
    }
    return false;
}

bool ReadWideZ(const std::vector<uint8_t>& file, size_t start, size_t end,
               std::wstring* out) {
    if (!out || start>=end || end>file.size()) return false;
    out->clear();
    for (size_t p=start;p+2u<=end && out->size()<8192u;p+=2u) {
        const uint16_t u=U16(file.data()+p);
        if (!u) return !out->empty();
        out->push_back(static_cast<wchar_t>(u));
    }
    return false;
}

bool HasLocExtension(const wchar_t* name) {
    if (!name) return false;
    const wchar_t* dot=wcsrchr(name,L'.');
    return dot && wcslen(dot)==4u && dot[0]==L'.' &&
           LowerAscii(dot[1])==L'l' && LowerAscii(dot[2])==L'o' &&
           LowerAscii(dot[3])==L'c';
}

std::string Upper(const std::string& value) {
    std::string result=value;
    for (char& c:result) c=UpperAscii(c);
    return result;
}

bool ContainsAny(const std::string& text,
                 std::initializer_list<const char*> needles) {
    for (const char* needle:needles) {
        if (text.find(needle)!=std::string::npos) return true;
    }
    return false;
}

bool HasNumberedToken(const std::string& upper, const char* prefix) {
    const std::string p(prefix);
    size_t at=0;
    while ((at=upper.find(p,at))!=std::string::npos) {
        size_t q=at+p.size();
        const size_t begin=q;
        while (q<upper.size() && upper[q]>='0' && upper[q]<='9') ++q;
        if (q>begin && q<upper.size() && upper[q]=='_') return true;
        at+=p.size();
    }
    return false;
}
}

const char* DialogueIndex::CategoryName(Category category) {
    switch(category) {
        case Category::Civilian: return "civilian";
        case Category::Background: return "background";
        case Category::Staff: return "staff";
        case Category::Guard: return "guard";
        case Category::RadioLike: return "radio";
        case Category::Other: return "other";
    }
    return "unknown";
}

bool DialogueIndex::IsScriptDialoguePath(const std::wstring& path) {
    const wchar_t* base=wcsrchr(path.c_str(),L'\\');
    base=base?base+1:path.c_str();
    std::wstring lower(base);
    for (wchar_t& c:lower) c=LowerAscii(c);
    return lower.size()>=19u &&
           lower.find(L"script_dialogue.loc")!=std::wstring::npos;
}

bool DialogueIndex::IsHardExcludedKey(const std::string& key) {
    if (key.empty()) return true;
    const std::string u=Upper(key);
    // These families are already owned by native story/tutorial/mission UI.
    // Keep them indexed for diagnostics, but never emit them as NPC overlay.
    return ContainsAny(u,{"BOND","NICOLE","DANTE","MON_DIA_",
                          "_TUTORIAL_","_INFO_","_CS_"});
}

DialogueIndex::Category DialogueIndex::ClassifyCategory(
    const std::string& key) {
    const std::string u=Upper(key);
    if (ContainsAny(u,{"DELTA","BRAVO","SQUAD","TEAM","RADIO"}))
        return Category::RadioLike;
    if (HasNumberedToken(u,"_PG") || u.find("GUARD")!=std::string::npos)
        return Category::Guard;
    if (HasNumberedToken(u,"_CM") || HasNumberedToken(u,"_CW") ||
        u.find("CIVILIAN")!=std::string::npos)
        return Category::Civilian;
    if (HasNumberedToken(u,"_BG")) return Category::Background;
    if (ContainsAny(u,{"BARMAID","BARMAN","WAITER","WORKER",
                       "FOREMAN","SCIENTIST"}))
        return Category::Staff;
    return Category::Other;
}

bool DialogueIndex::IsAllowedByPolicy(Category category,
                                          const Config& config) {
    switch (category) {
        case Category::Civilian: return true;
        case Category::Background: return true;
        case Category::Staff: return true;
        case Category::Guard: return config.show_guards;
        case Category::RadioLike: return config.show_radio_like;
        case Category::Other: return false;
    }
    return false;
}

void DialogueIndex::CollectLocFiles(
    const std::wstring& directory,unsigned depth,
    std::vector<std::wstring>* files) const {
    if (!files || depth>kMaximumLocDepth) return;
    const std::wstring pattern=JoinPath(directory,L"*");
    WIN32_FIND_DATAW data{};
    HANDLE find=FindFirstFileW(pattern.c_str(),&data);
    if (find==INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(data.cFileName,L".")==0 || wcscmp(data.cFileName,L"..")==0)
            continue;
        const std::wstring path=JoinPath(directory,data.cFileName);
        if (data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) {
            if (!(data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))
                CollectLocFiles(path,depth+1u,files);
        } else if (HasLocExtension(data.cFileName) && IsScriptDialoguePath(path)) {
            std::wstring lower_path(path);
            for (wchar_t& c:lower_path) c=LowerAscii(c);
            if (lower_path.find(L"\\testlevels\\")==std::wstring::npos)
                files->push_back(path);
        }
    } while (FindNextFileW(find,&data)!=FALSE);
    FindClose(find);
}

bool DialogueIndex::LoadScriptDialogueLoc(
    const std::wstring& path,const Config& config,size_t* parsed_entries,
    size_t* accepted_entries) {
    std::vector<uint8_t> file;
    const auto fail = [&](const char* reason, uint32_t key_hint = 0u,
                          uint32_t lang_block = 0u) {
        NPC_LOG_WARN(
            "NPC_LOC_PARSE_FAIL file=%s reason=%s key_hint=0x%08X lang_block=0x%08X",
            WideToUtf8(path).c_str(), reason, key_hint, lang_block);
        return false;
    };
    if (!ReadWholeFile(path,&file)) return fail("READ_FILE");
    if (file.size()<kLanguageDirectory+kLanguageRecordSize*kLanguageCount)
        return fail("FILE_TOO_SMALL");
    if (memcmp(file.data(),"OLTX",4)!=0) return fail("BAD_MAGIC");

    if (config.loc_language.size()!=2u) return fail("BAD_LANGUAGE_CONFIG");
    const wchar_t lang0=LowerAscii(config.loc_language[0]);
    const wchar_t lang1=LowerAscii(config.loc_language[1]);
    uint32_t lang_block=0,lang_size=0;
    size_t first_lang_block=file.size();
    for (size_t i=0;i<kLanguageCount;++i) {
        const size_t off=kLanguageDirectory+i*kLanguageRecordSize;
        const uint32_t block=U32(file.data()+off+16u);
        const uint32_t size=U32(file.data()+off+20u);
        if (block && size>=8u && static_cast<uint64_t>(block)+size<=file.size()) {
            first_lang_block=std::min(first_lang_block,static_cast<size_t>(block));
        }
        if (LowerAscii(static_cast<wchar_t>(file[off]))==lang0 &&
            LowerAscii(static_cast<wchar_t>(file[off+1]))==lang1) {
            lang_block=block; lang_size=size;
        }
    }
    if (!lang_block || lang_size<8u ||
        static_cast<uint64_t>(lang_block)+lang_size>file.size())
        return fail("LANG_BLOCK_INVALID",U32(file.data()+0x18u),lang_block);
    if (first_lang_block==file.size())
        return fail("NO_VALID_LANGUAGE_BLOCK",U32(file.data()+0x18u),lang_block);

    const uint32_t lang_count=U32(file.data()+lang_block);
    const uint32_t lang_stride=U32(file.data()+lang_block+4u);
    if (lang_stride!=8u || !lang_count || lang_count>kMaximumEntries ||
        static_cast<uint64_t>(lang_block)+8ull+8ull*lang_count>
            static_cast<uint64_t>(lang_block)+lang_size)
        return fail("LANG_TABLE_INVALID",U32(file.data()+0x18u),lang_block);

    std::unordered_map<uint32_t,uint32_t> text_offsets;
    text_offsets.reserve(lang_count);
    for (uint32_t i=0;i<lang_count;++i) {
        const size_t e=static_cast<size_t>(lang_block)+8u+8u*i;
        const uint32_t hash=U32(file.data()+e);
        const uint32_t rel=U32(file.data()+e+4u);
        const uint64_t text_start=static_cast<uint64_t>(lang_block)+rel;
        if (text_start>=static_cast<uint64_t>(lang_block)+lang_size) continue;
        text_offsets[hash]=rel;
    }
    if (text_offsets.empty())
        return fail("LANG_TEXT_OFFSETS_EMPTY",U32(file.data()+0x18u),lang_block);

    // OLTX key-table discovery must not assume that header+0x18 is an absolute
    // file offset. Historical round-trip samples expose a real layout where
    // header[0x18]=0xCC while the key block starts at 0xDC.  R10 treated 0xCC
    // as absolute and therefore rejected every script_dialogue file before the
    // Dialogue hook could even be installed.  Validate several format-derived
    // hints, then scan the narrow directory->first-language gap as a fallback.
    const size_t directory_end=kLanguageDirectory+kLanguageRecordSize*kLanguageCount;
    const uint32_t key_hint=U32(file.data()+0x18u);
    const uint32_t header_base=U32(file.data()+0x08u);

    auto plausible_key_block = [&](size_t candidate,uint32_t* count_out) {
        if (candidate<directory_end || candidate+8u>first_lang_block ||
            candidate+8u>file.size()) return false;
        const uint32_t count=U32(file.data()+candidate);
        const uint32_t stride=U32(file.data()+candidate+4u);
        if (!count || count>kMaximumEntries || stride!=8u) return false;
        const uint64_t table_end64=static_cast<uint64_t>(candidate)+8ull+8ull*count;
        if (table_end64>first_lang_block || table_end64>file.size()) return false;
        const size_t table_end=static_cast<size_t>(table_end64);
        size_t readable=0,hash_overlap=0;
        for (uint32_t i=0;i<count;++i) {
            const size_t e=candidate+8u+8u*i;
            const uint32_t hash=U32(file.data()+e);
            const uint32_t rel=U32(file.data()+e+4u);
            const uint64_t key_start64=static_cast<uint64_t>(candidate)+rel;
            if (key_start64<table_end || key_start64>=first_lang_block) return false;
            std::string key;
            if (!ReadAsciiZ(file,static_cast<size_t>(key_start64),first_lang_block,&key))
                return false;
            ++readable;
            if (text_offsets.find(hash)!=text_offsets.end()) ++hash_overlap;
        }
        if (readable!=count || hash_overlap==0u) return false;
        if (count_out) *count_out=count;
        return true;
    };

    size_t key_block=0;
    uint32_t key_count=0;
    const char* discovery="NONE";
    auto try_candidate = [&](uint64_t candidate,const char* mode) {
        if (key_block || candidate>static_cast<uint64_t>(SIZE_MAX)) return;
        uint32_t count=0;
        if (plausible_key_block(static_cast<size_t>(candidate),&count)) {
            key_block=static_cast<size_t>(candidate);
            key_count=count;
            discovery=mode;
        }
    };
    try_candidate(key_hint,"HEADER_ABSOLUTE");
    try_candidate(static_cast<uint64_t>(key_hint)+header_base,"HEADER_PLUS_BASE");
    try_candidate(directory_end,"DIRECTORY_END");
    if (!key_block && first_lang_block>=directory_end+8u) {
        for (size_t candidate=directory_end;
             candidate+8u<=first_lang_block; candidate+=4u) {
            uint32_t count=0;
            if (plausible_key_block(candidate,&count)) {
                key_block=candidate; key_count=count; discovery="VALIDATED_SCAN";
                break;
            }
        }
    }
    if (!key_block)
        return fail("KEY_BLOCK_NOT_FOUND",key_hint,lang_block);

    if (strcmp(discovery,"HEADER_ABSOLUTE")!=0) {
        NPC_LOG_INFO(
            "NPC_LOC_KEY_TABLE_RECOVERED file=%s mode=%s key_hint=0x%08X header_base=0x%08X key_block=0x%08X first_lang=0x%08X entries=%u",
            WideToUtf8(path).c_str(),discovery,key_hint,header_base,
            static_cast<unsigned>(key_block),
            static_cast<unsigned>(first_lang_block),key_count);
    }

    const size_t key_end=first_lang_block;
    const size_t lang_end=static_cast<size_t>(lang_block)+lang_size;
    size_t local_parsed=0,local_accepted=0;
    for (uint32_t i=0;i<key_count;++i) {
        const size_t e=key_block+8u+8u*i;
        const uint32_t hash=U32(file.data()+e);
        const uint32_t key_rel=U32(file.data()+e+4u);
        const uint64_t key_start64=static_cast<uint64_t>(key_block)+key_rel;
        const auto text_it=text_offsets.find(hash);
        if (key_start64>=key_end || text_it==text_offsets.end()) continue;
        const uint64_t text_start64=static_cast<uint64_t>(lang_block)+text_it->second;
        if (text_start64>=lang_end) continue;
        std::string key;
        std::wstring text;
        if (!ReadAsciiZ(file,static_cast<size_t>(key_start64),key_end,&key) ||
            !ReadWideZ(file,static_cast<size_t>(text_start64),lang_end,&text)) continue;
        ++local_parsed;
        if (parsed_entries) ++*parsed_entries;
        const Category category=ClassifyCategory(key);
        const bool hard_excluded=IsHardExcludedKey(key);
        const bool policy_allowed=!hard_excluded && (config.coverage_first || IsAllowedByPolicy(category,config));
        if (policy_allowed) {
            ++local_accepted;
            if (accepted_entries) ++*accepted_entries;
        }

        auto existing=by_hash_.find(hash);
        if (existing!=by_hash_.end()) {
            const Record& old=records_[existing->second];
            if (old.text!=text || old.key_name!=key) ambiguous_hashes_.insert(hash);
            continue;
        }
        Record record{};
        record.hash=hash; record.key_name=std::move(key); record.source_path=path;
        record.text=std::move(text); record.category=category;
        record.policy_allowed=policy_allowed; record.hard_excluded=hard_excluded;
        by_hash_[hash]=records_.size();
        records_.push_back(std::move(record));
    }
    if (!local_parsed) return fail("NO_PARSED_KEYS",key_hint,lang_block);
    NPC_LOG_INFO(
        "NPC_LOC_PARSE_OK file=%s key_mode=%s parsed=%u policy_allowed=%u",
        WideToUtf8(path).c_str(),discovery,
        static_cast<unsigned>(local_parsed),static_cast<unsigned>(local_accepted));
    return true;
}

bool DialogueIndex::Initialize(const std::wstring& module_directory,
                                  const Config& config) {
    Shutdown();
    if (!config.enable) return true;
    if (config.battle_subtitles) {
        NPC_LOG_WARN("NPC_BATTLE_SUBTITLES_FORCED_OFF reason=SEALED_UNMAPPED_PATH");
    }
    const std::wstring root=JoinPath(module_directory,config.bscn_root + L"\\loc");
    std::vector<std::wstring> files;
    CollectLocFiles(root,0,&files);
    std::sort(files.begin(),files.end());
    size_t parsed=0,accepted=0,valid=0;
    for (const std::wstring& file:files) {
        if (LoadScriptDialogueLoc(file,config,&parsed,&accepted)) ++valid;
    }
    if (!ambiguous_hashes_.empty()) {
        std::vector<Record> clean;
        clean.reserve(records_.size());
        by_hash_.clear();
        for (Record& record:records_) {
            if (ambiguous_hashes_.find(record.hash)!=ambiguous_hashes_.end()) continue;
            by_hash_[record.hash]=clean.size();
            clean.push_back(std::move(record));
        }
        records_.swap(clean);
    }
    NPC_LOG_INFO(
        "NPC_DIALOGUE_INDEX_READY files=%u/%u parsed=%u policy_allowed=%u records=%u ambiguous_hashes=%u source=BSCNContents\\loc policy=INDEX_ALL_THEN_RUNTIME_FILTER partial=PARTIAL_PARSE_DOES_NOT_DISABLE coverage_first=%u",
        static_cast<unsigned>(valid),static_cast<unsigned>(files.size()),
        static_cast<unsigned>(parsed),static_cast<unsigned>(accepted),
        static_cast<unsigned>(records_.size()),
        static_cast<unsigned>(ambiguous_hashes_.size()),
        config.coverage_first ? 1u : 0u);
    if (valid!=files.size()) {
        NPC_LOG_WARN(
            "NPC_DIALOGUE_INDEX_PARTIAL files=%u/%u action=KEEP_VALID_RECORDS",
            static_cast<unsigned>(valid),static_cast<unsigned>(files.size()));
    }
    return valid>0u && !records_.empty();
}

const DialogueIndex::Record* DialogueIndex::Find(uint32_t hash) const {
    const auto found=by_hash_.find(hash);
    return found==by_hash_.end()?nullptr:&records_[found->second];
}

void DialogueIndex::Shutdown() {
    records_.clear(); by_hash_.clear(); ambiguous_hashes_.clear();
}
}
