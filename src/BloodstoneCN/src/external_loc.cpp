#include "external_loc.h"
#include "log.h"

namespace bscn {
namespace {
constexpr uint32_t kBliVersion = 3;
constexpr uint32_t kBliHeaderSize = 32;
constexpr uint32_t kMaximumUnits = 8192;
constexpr DWORD kDeferredWindowMs = 2000;
constexpr DWORD kRecentPreciseWindowMs = 10000;
constexpr DWORD kCrossThreadDeferredWindowMs = 10000;
constexpr unsigned kMaximumLocDepth = 32;

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

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>* out) {
    if (!out) return false;
    out->clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0 &&
              size.QuadPart <= 64ll * 1024ll * 1024ll;
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        size_t position = 0;
        while (position < out->size()) {
            const DWORD want = static_cast<DWORD>(std::min<size_t>(
                out->size() - position, static_cast<size_t>(1u << 20)));
            DWORD got = 0;
            if (!ReadFile(file, out->data() + position, want, &got, nullptr) ||
                got != want) {
                ok = false;
                break;
            }
            position += got;
        }
    }
    CloseHandle(file);
    if (!ok) out->clear();
    return ok;
}

std::wstring ReadWide(const uint8_t* p, uint32_t units) {
    std::wstring text;
    text.reserve(units);
    for (uint32_t index = 0; index < units; ++index) {
        text.push_back(static_cast<wchar_t>(U16(p + index * 2u)));
    }
    return text;
}

wchar_t LowerAscii(wchar_t value) {
    return value >= L'A' && value <= L'Z'
               ? static_cast<wchar_t>(value - L'A' + L'a')
               : value;
}

bool HasLocExtension(const wchar_t* name) {
    if (!name) return false;
    const wchar_t* dot = wcsrchr(name, L'.');
    if (!dot || wcslen(dot) != 4u) return false;
    return dot[0] == L'.' && LowerAscii(dot[1]) == L'l' &&
           LowerAscii(dot[2]) == L'o' && LowerAscii(dot[3]) == L'c';
}

bool IsAsciiTokenNameUnit(wchar_t value) {
    return value >= 0x21 && value <= 0x7E && value != L'{' &&
           value != L'}' && value != L'~';
}

bool IsAsciiSpace(wchar_t value) {
    return value == L' ' || value == L'\t' || value == L'\r' ||
           value == L'\n';
}


bool HasCjkSignal(const std::wstring& text) {
    for (wchar_t value : text) {
        const uint32_t u = static_cast<uint32_t>(value);
        if ((u >= 0x3400u && u <= 0x4DBFu) ||
            (u >= 0x4E00u && u <= 0x9FFFu) ||
            (u >= 0xF900u && u <= 0xFAFFu)) {
            return true;
        }
    }
    return false;
}

bool IsWrappedTokenNameUnit(wchar_t value) {
    return value >= 0x21 && value <= 0x7E &&
           value != L'{' && value != L'}' && value != L'~';
}

bool HasWrappedTokenAt(const std::wstring& input, size_t begin) {
    if (begin + 7u > input.size() || input[begin] != L'~' ||
        input[begin + 1u] != L'~' || input[begin + 2u] != L'{') {
        return false;
    }
    size_t cursor = begin + 3u;
    const size_t name_begin = cursor;
    while (cursor < input.size() && cursor - name_begin <= 64u) {
        if (input[cursor] == L'}') {
            return cursor > name_begin && cursor + 2u < input.size() &&
                   input[cursor + 1u] == L'~' && input[cursor + 2u] == L'~';
        }
        if (!IsWrappedTokenNameUnit(input[cursor])) return false;
        ++cursor;
    }
    return false;
}

bool HasWrappedTokenEndingAt(const std::wstring& input, size_t end) {
    if (end < 7u || input[end - 3u] != L'}' || input[end - 2u] != L'~' ||
        input[end - 1u] != L'~') {
        return false;
    }
    const size_t earliest = end > 70u ? end - 70u : 0u;
    for (size_t begin = end - 3u; begin > earliest;) {
        --begin;
        if (input[begin] != L'{') continue;
        if (begin < 2u || input[begin - 1u] != L'~' ||
            input[begin - 2u] != L'~') {
            continue;
        }
        return HasWrappedTokenAt(input, begin - 2u);
    }
    return false;
}

// 只允许把原 key/hash 精确确认的 source 嵌回“完整结构字段”，不再做普通
// find/replace。冒号后的 Speaker body 始终属于强字段；破折号/斜杠字段只有
// 当前串前部已经本地化时才开放，用于“中文 carrier：English tail”收尾。
size_t FindStructuredOccurrence(const std::wstring& input,
                                const std::wstring& needle) {
    if (needle.empty() || needle.size() > input.size()) return std::wstring::npos;
    if (input == needle) return 0u;
    const bool localized_prefix_allowed = HasCjkSignal(input);
    size_t selected = std::wstring::npos;
    size_t search = 0u;
    while (search < input.size()) {
        const size_t at = input.find(needle, search);
        if (at == std::wstring::npos) break;
        size_t before = at;
        while (before > 0u && IsAsciiSpace(input[before - 1u])) --before;
        bool strong_before = false;
        if (before == 0u) {
            strong_before = true;
        } else if (HasWrappedTokenEndingAt(input, before)) {
            // AP1 / UIContextMoves 的实时按键 token 是结构边界。这里只运行在
            // 原 key/hash 已精确确认的 recent/deferred mapping 上，因此允许
            // “~~{TOKEN}~~   body” 中的完整 body 被替换，而不是普通 substring。
            strong_before = true;
        } else {
            const wchar_t marker = input[before - 1u];
            if (marker == L':' || marker == static_cast<wchar_t>(0xFF1A)) {
                strong_before = true;
            } else if (localized_prefix_allowed &&
                       (marker == L'-' || marker == L'/')) {
                strong_before = true;
            }
        }

        size_t after = at + needle.size();
        while (after < input.size() && IsAsciiSpace(input[after])) ++after;
        const bool strong_after =
            after == input.size() || HasWrappedTokenAt(input, after) ||
            input[after] == L':' ||
            input[after] == static_cast<wchar_t>(0xFF1A) ||
            (localized_prefix_allowed &&
             (input[after] == L'-' || input[after] == L'/'));

        if (strong_before && strong_after) {
            if (selected != std::wstring::npos) return std::wstring::npos;
            selected = at;
        }
        search = at + 1u;
    }
    return selected;
}
}  // namespace

ExternalLocOverrides::DeferredContext& ExternalLocOverrides::ThreadContext() {
    static thread_local DeferredContext context;
    return context;
}

ExternalLocOverrides::PreciseThreadContext&
ExternalLocOverrides::PreciseContext() {
    static thread_local PreciseThreadContext context;
    return context;
}

ExternalLocOverrides::CrossThreadOutputContext&
ExternalLocOverrides::CrossThreadContext() {
    static thread_local CrossThreadOutputContext context;
    return context;
}

bool ExternalLocOverrides::Initialize(const std::wstring& module_directory,
                                      const Config& config) {
    Shutdown();
    if (!config.enable_external_loc) return true;

    hud_bypass_dynamic_scalars_ = config.hud_bypass_dynamic_scalars;
    hud_defer_technical_labels_ = config.hud_defer_technical_labels;
    root_ = JoinPath(module_directory, config.external_loc_root);
    index_path_ = JoinPath(module_directory, config.index_file);
    if (config.external_loc_language.size() != 2u) {
        BSCN_LOG_ERROR(
            "ExternalLoc Language must be exactly two ASCII characters");
        return false;
    }
    language_[0] = LowerAscii(config.external_loc_language[0]);
    language_[1] = LowerAscii(config.external_loc_language[1]);

    std::vector<std::wstring> files;
    const DWORD attributes = GetFileAttributesW(root_.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        BSCN_LOG_ERROR("ExternalLoc root missing path=%s",
                       WideToUtf8(root_).c_str());
        return false;
    }
    CollectLocFiles(root_, 0, &files);
    std::sort(files.begin(), files.end());
    loc_file_count_ = files.size();
    if (files.empty()) {
        BSCN_LOG_ERROR("ExternalLoc root contains no .loc files path=%s",
                       WideToUtf8(root_).c_str());
        return false;
    }
    for (const std::wstring& path : files) {
        if (LoadLocFile(path)) ++valid_loc_file_count_;
    }
    if (valid_loc_file_count_ != loc_file_count_) {
        BSCN_LOG_ERROR(
            "ExternalLoc incomplete files=%u/%u path=%s",
            static_cast<unsigned>(valid_loc_file_count_),
            static_cast<unsigned>(loc_file_count_),
            WideToUtf8(root_).c_str());
        return false;
    }

    const bool index_loaded = LoadIndex(index_path_);
    if (!index_loaded && config.require_index) {
        BSCN_LOG_ERROR("BSCNLI3 load failed path=%s",
                       WideToUtf8(index_path_).c_str());
        return false;
    }

    enabled_ = true;
    size_t ambiguous_hashes = 0;
    for (const auto& item : by_hash_) {
        if (item.second.size() > 1u) ++ambiguous_hashes;
    }
    size_t loc_multi_value_hashes = 0;
    size_t loc_total_values = 0;
    for (const auto& item : loc_candidates_) {
        loc_total_values += item.second.size();
        if (item.second.size() > 1u) ++loc_multi_value_hashes;
    }
    BSCN_LOG_INFO(
        "ExternalLoc overlay ready root=%s language=%c%c files=%u/%u loc_hashes=%u loc_values=%u loc_multi_value_hashes=%u index=%u records=%u ambiguous_hashes=%u PAK_fallback=1",
        WideToUtf8(root_).c_str(), static_cast<char>(language_[0]),
        static_cast<char>(language_[1]),
        static_cast<unsigned>(valid_loc_file_count_),
        static_cast<unsigned>(loc_file_count_),
        static_cast<unsigned>(loc_overrides_.size()),
        static_cast<unsigned>(loc_total_values),
        static_cast<unsigned>(loc_multi_value_hashes),
        index_loaded ? 1u : 0u,
        static_cast<unsigned>(records_.size()),
        static_cast<unsigned>(ambiguous_hashes));
    return true;
}

void ExternalLocOverrides::SetRenderingReady(bool ready) {
    const bool previous = rendering_ready_.exchange(ready);
    if (ready && !previous) {
        font_gate_wait_logged_.store(false);
        BSCN_LOG_INFO("FONT_GATE_READY localization_output=ENABLED");
    } else if (!ready && previous) {
        BSCN_LOG_INFO("FONT_GATE_WAIT localization_output=PAUSED reason=ATLAS_NOT_READY");
    }
}

void ExternalLocOverrides::CollectLocFiles(
    const std::wstring& directory, unsigned depth,
    std::vector<std::wstring>* files) const {
    if (!files || depth > kMaximumLocDepth) return;
    const std::wstring pattern = JoinPath(directory, L"*");
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(data.cFileName, L".") == 0 ||
            wcscmp(data.cFileName, L"..") == 0) {
            continue;
        }
        const std::wstring path = JoinPath(directory, data.cFileName);
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                CollectLocFiles(path, depth + 1u, files);
            }
        } else if (HasLocExtension(data.cFileName)) {
            files->push_back(path);
        }
    } while (FindNextFileW(find, &data) != FALSE);
    FindClose(find);
}

bool ExternalLocOverrides::LoadLocFile(const std::wstring& path) {
    std::vector<uint8_t> file;
    constexpr size_t kLanguageDirectory = 0x1C;
    constexpr size_t kLanguageRecordSize = 24;
    constexpr size_t kLanguageCount = 8;
    constexpr size_t kMinimumSize =
        kLanguageDirectory + kLanguageRecordSize * kLanguageCount;
    if (!ReadWholeFile(path, &file) || file.size() < kMinimumSize ||
        memcmp(file.data(), "OLTX", 4) != 0) {
        BSCN_LOG_WARN("ExternalLoc rejected invalid file=%s",
                      WideToUtf8(path).c_str());
        return false;
    }

    uint32_t block_offset = 0;
    uint32_t block_size = 0;
    for (size_t index = 0; index < kLanguageCount; ++index) {
        const size_t offset = kLanguageDirectory + index * kLanguageRecordSize;
        const wchar_t first = LowerAscii(file[offset]);
        const wchar_t second = LowerAscii(file[offset + 1]);
        if (first == language_[0] && second == language_[1]) {
            block_offset = U32(file.data() + offset + 16);
            block_size = U32(file.data() + offset + 20);
            break;
        }
    }
    const uint64_t block_end64 =
        static_cast<uint64_t>(block_offset) + block_size;
    if (!block_offset || block_size < 8u || block_end64 > file.size()) {
        BSCN_LOG_WARN(
            "ExternalLoc missing/invalid language block file=%s language=%c%c",
            WideToUtf8(path).c_str(), static_cast<char>(language_[0]),
            static_cast<char>(language_[1]));
        return false;
    }

    const size_t block_end = static_cast<size_t>(block_end64);
    const uint32_t count = U32(file.data() + block_offset);
    const uint32_t stride = U32(file.data() + block_offset + 4u);
    const uint64_t table_end = static_cast<uint64_t>(block_offset) + 8u +
        static_cast<uint64_t>(count) * 8u;
    if (stride != 8u || count > 200000u || table_end > block_end) {
        BSCN_LOG_WARN("ExternalLoc invalid string table file=%s",
                      WideToUtf8(path).c_str());
        return false;
    }

    for (uint32_t index = 0; index < count; ++index) {
        const size_t entry = static_cast<size_t>(block_offset) + 8u +
            static_cast<size_t>(index) * 8u;
        const uint32_t key_hash = U32(file.data() + entry);
        const uint32_t relative = U32(file.data() + entry + 4u);
        const uint64_t start64 = static_cast<uint64_t>(block_offset) + relative;
        if (start64 >= block_end) continue;

        size_t position = static_cast<size_t>(start64);
        std::wstring text;
        text.reserve(64);
        bool terminated = false;
        while (position + 2u <= block_end && text.size() < kMaximumUnits) {
            const uint16_t unit = U16(file.data() + position);
            position += 2u;
            if (unit == 0u) {
                terminated = true;
                break;
            }
            text.push_back(static_cast<wchar_t>(unit));
        }
        if (terminated && !text.empty()) {
            auto& candidates = loc_candidates_[key_hash];
            if (std::find(candidates.begin(), candidates.end(), text) ==
                candidates.end()) {
                candidates.push_back(text);
            }
            // 历史 loose hash fallback 仍保持最后值语义；真正需要 source
            // context 的路径改由 loc_candidates_ + BLI3 source/hash 消歧。
            loc_overrides_[key_hash] = std::move(text);
        }
    }
    return true;
}

bool ExternalLocOverrides::LoadIndex(const std::wstring& path) {
    std::vector<uint8_t> file;
    if (!ReadWholeFile(path, &file) || file.size() < kBliHeaderSize) return false;
    static constexpr char magic[8] = {'B','S','C','N','L','I','3','\0'};
    if (memcmp(file.data(), magic, 8) != 0 ||
        U32(file.data() + 8) != kBliVersion ||
        U32(file.data() + 12) != kBliHeaderSize) {
        return false;
    }
    const uint32_t count = U32(file.data() + 16);
    const uint32_t payload = U32(file.data() + 20);
    if (!count || count > 200000u || payload != file.size() - kBliHeaderSize)
        return false;

    records_.reserve(count);
    size_t position = kBliHeaderSize;
    size_t deferred = 0;
    for (uint32_t index = 0; index < count; ++index) {
        if (position + 16u > file.size()) return false;
        IndexedOverride record;
        record.key_hash = U32(file.data() + position);
        const uint32_t source_units = U32(file.data() + position + 4u);
        const uint32_t replacement_units = U32(file.data() + position + 8u);
        record.flags = U32(file.data() + position + 12u);
        position += 16u;
        if ((record.flags & ~kIndexFlagDeferToSubmitText) != 0 ||
            source_units == 0u || replacement_units == 0u ||
            source_units > kMaximumUnits || replacement_units > kMaximumUnits) {
            return false;
        }
        const uint64_t bytes =
            static_cast<uint64_t>(source_units + replacement_units) * 2u;
        if (bytes > file.size() - position) return false;
        record.source = ReadWide(file.data() + position, source_units);
        position += static_cast<size_t>(source_units) * 2u;
        record.replacement = ReadWide(file.data() + position, replacement_units);
        position += static_cast<size_t>(replacement_units) * 2u;
        if ((record.flags & kIndexFlagDeferToSubmitText) != 0) ++deferred;
        const size_t record_index = records_.size();
        records_.push_back(std::move(record));
        by_hash_[records_.back().key_hash].push_back(record_index);
        by_source_[records_.back().source].push_back(record_index);
    }
    if (position != file.size()) return false;

    size_t ambiguous = 0;
    for (const auto& item : by_hash_) {
        if (item.second.size() > 1u) ++ambiguous;
    }
    BSCN_LOG_INFO(
        "UNICODE_LOC_INDEX_OK path=%s format=BSCNLI3 records=%u deferred=%u hashes=%u ambiguous_hashes=%u",
        WideToUtf8(path).c_str(), static_cast<unsigned>(records_.size()),
        static_cast<unsigned>(deferred), static_cast<unsigned>(by_hash_.size()),
        static_cast<unsigned>(ambiguous));
    return true;
}

const ExternalLocOverrides::IndexedOverride*
ExternalLocOverrides::SelectHashFallbackCandidate(
    uint32_t hash, const wchar_t* original) {
    const auto found = by_hash_.find(hash);
    if (found == by_hash_.end()) return nullptr;
    const auto& indices = found->second;
    const IndexedOverride* selected = nullptr;
    for (size_t index : indices) {
        const IndexedOverride& candidate = records_[index];
        if (original && *original && candidate.source != original &&
            candidate.replacement != original) {
            continue;
        }
        if (selected) {
            // Hash fallback is only for sources absent from the source index.
            // More than one source-compatible record is not deterministic.
            return nullptr;
        }
        selected = &candidate;
    }
    if (selected) return selected;

    bool log_it = false;
    {
        std::lock_guard<std::mutex> lock(source_mismatch_log_mutex_);
        if (logged_source_mismatch_hashes_.size() < 64u)
            log_it = logged_source_mismatch_hashes_.insert(hash).second;
    }
    if (log_it && original && *original) {
        BSCN_LOG_WARN(
            "UNICODE_LOC_SOURCE_MISMATCH hash=0x%08X original=%s candidates=%u action=KEEP_ORIGINAL",
            hash, WideToUtf8(original).c_str(),
            static_cast<unsigned>(indices.size()));
    }
    return nullptr;
}

const ExternalLocOverrides::IndexedOverride*
ExternalLocOverrides::SelectSourceFirstCandidate(
    uint32_t primary_hash, uint32_t secondary_hash, const wchar_t* original) {
    if (!original || !*original) {
        const uint32_t hash = primary_hash ? primary_hash : secondary_hash;
        return hash ? SelectHashFallbackCandidate(hash, original) : nullptr;
    }

    const auto source_found = by_source_.find(std::wstring(original));
    if (source_found == by_source_.end()) {
        // Compatibility fallback for a source form not represented exactly in
        // BLI3. The hash path remains source-validated, so it cannot cross to a
        // different source merely because a cached hash is stale.
        if (primary_hash) {
            if (const IndexedOverride* candidate =
                    SelectHashFallbackCandidate(primary_hash, original)) {
                return candidate;
            }
        }
        if (secondary_hash && secondary_hash != primary_hash) {
            return SelectHashFallbackCandidate(secondary_hash, original);
        }
        return nullptr;
    }

    const auto& family = source_found->second;
    if (family.empty()) return nullptr;
    if (family.size() == 1u) {
        const IndexedOverride& only = records_[family.front()];
        const bool any_hash = primary_hash != 0u || secondary_hash != 0u;
        const bool hash_matches =
            (primary_hash != 0u && only.key_hash == primary_hash) ||
            (secondary_hash != 0u && only.key_hash == secondary_hash);
        const bool hash_disagrees = any_hash && !hash_matches;
        if (hash_disagrees) {
            const uint64_t key =
                (static_cast<uint64_t>(primary_hash) << 32u) ^ secondary_hash ^
                static_cast<uint64_t>(only.key_hash);
            bool log_it=false;
            {
                std::lock_guard<std::mutex> lock(source_first_log_mutex_);
                if (logged_source_first_disagreements_.size() < 128u)
                    log_it=logged_source_first_disagreements_.insert(key).second;
            }
            if (log_it) {
                BSCN_LOG_INFO(
                    "SOURCE_FIRST_HASH_DISAGREEMENT primary=0x%08X secondary=0x%08X selected_key_hash=0x%08X original=%s policy=SOURCE_IDENTITY_WINS",
                    primary_hash, secondary_hash, only.key_hash,
                    WideToUtf8(original).c_str());
            }
        }
        return &only;
    }

    auto select_hash_inside_family = [&](uint32_t hash)
        -> const IndexedOverride* {
        if (!hash) return nullptr;
        const IndexedOverride* selected = nullptr;
        for (size_t index : family) {
            const IndexedOverride& candidate = records_[index];
            if (candidate.key_hash != hash) continue;
            if (selected) {
                // Multiple records with the same source+hash are only safe when
                // their current LOC result and defer behavior are identical.
                const std::wstring* a = FindAuthoritativeReplacement(*selected);
                const std::wstring* b = FindAuthoritativeReplacement(candidate);
                if (!a || !b || *a != *b || selected->flags != candidate.flags)
                    return nullptr;
            } else {
                selected = &candidate;
            }
        }
        return selected;
    };

    if (const IndexedOverride* candidate =
            select_hash_inside_family(primary_hash)) return candidate;
    if (secondary_hash != primary_hash) {
        if (const IndexedOverride* candidate =
                select_hash_inside_family(secondary_hash)) return candidate;
    }

    // Hash may be stale/reused. We may still proceed only if every record in
    // this exact source family has the same authoritative current LOC output
    // and the same defer semantics. This is agreement, not first-candidate.
    const IndexedOverride* representative = nullptr;
    const std::wstring* agreed_replacement = nullptr;
    uint32_t agreed_flags = 0;
    for (size_t index : family) {
        const IndexedOverride& candidate = records_[index];
        const std::wstring* authoritative =
            FindAuthoritativeReplacement(candidate);
        if (!authoritative) return nullptr;
        if (!representative) {
            representative = &candidate;
            agreed_replacement = authoritative;
            agreed_flags = candidate.flags;
        } else if (*authoritative != *agreed_replacement ||
                   candidate.flags != agreed_flags) {
            bool expected=false;
            if (ambiguous_miss_logged_.compare_exchange_strong(expected,true)) {
                BSCN_LOG_WARN(
                    "UNICODE_LOC_SOURCE_FAMILY_AMBIGUOUS source=%s records=%u primary=0x%08X secondary=0x%08X policy=NO_GUESS",
                    WideToUtf8(original).c_str(),
                    static_cast<unsigned>(family.size()), primary_hash,
                    secondary_hash);
            }
            return nullptr;
        }
    }

    if (representative) {
        const uint64_t key =
            (static_cast<uint64_t>(primary_hash) << 32u) ^ secondary_hash ^
            static_cast<uint64_t>(representative->key_hash);
        bool log_it=false;
        {
            std::lock_guard<std::mutex> lock(source_first_log_mutex_);
            if (logged_source_first_disagreements_.size() < 128u)
                log_it=logged_source_first_disagreements_.insert(key).second;
        }
        if (log_it) {
            BSCN_LOG_INFO(
                "SOURCE_FIRST_HASH_DISAGREEMENT primary=0x%08X secondary=0x%08X selected_key_hash=0x%08X source_family_records=%u original=%s policy=SOURCE_FAMILY_UNIQUE_AGREEMENT",
                primary_hash, secondary_hash, representative->key_hash,
                static_cast<unsigned>(family.size()),
                WideToUtf8(original).c_str());
        }
    }
    return representative;
}

void ExternalLocOverrides::LogAuthoritativeLocProblem(
    uint32_t key_hash, size_t candidate_count, const char* reason) const {
    bool inserted = false;
    size_t total = 0;
    {
        std::lock_guard<std::mutex> lock(authoritative_log_mutex_);
        if (logged_authoritative_problem_hashes_.size() < 32u) {
            inserted = logged_authoritative_problem_hashes_.insert(key_hash).second;
            total = logged_authoritative_problem_hashes_.size();
        }
    }
    if (inserted) {
        BSCN_LOG_WARN(
            "AUTHORITATIVE_LOC_UNRESOLVED unique=%u hash=0x%08X candidates=%u reason=%s action=NO_GUESS",
            static_cast<unsigned>(total), key_hash,
            static_cast<unsigned>(candidate_count), reason ? reason : "UNKNOWN");
    }
}

std::wstring ExternalLocOverrides::CopyAuthoritativeReplacement(
    uint32_t key_hash, const std::wstring& indexed_replacement) const {
    const auto values = loc_candidates_.find(key_hash);
    // R7 的硬约束：运行时译文只能来自当前 86 个 loose LOC。
    // BLI3 的 replacement 仅作为历史语境指纹，不能在当前 LOC 缺项时
    // 反过来充当正文来源；缺项直接 NO_GUESS，日志会暴露资源/索引不一致。
    if (values == loc_candidates_.end() || values->second.empty()) {
        LogAuthoritativeLocProblem(key_hash, 0u, "LOC_HASH_MISSING");
        return std::wstring();
    }

    const auto& candidates = values->second;
    if (candidates.size() == 1u) return candidates.front();

    for (const std::wstring& candidate : candidates) {
        if (candidate == indexed_replacement) return candidate;
    }

    LogAuthoritativeLocProblem(key_hash, candidates.size(),
                               "MULTI_VALUE_CONTEXT_MISMATCH");
    return std::wstring();
}

const std::wstring* ExternalLocOverrides::FindAuthoritativeReplacement(
    const IndexedOverride& candidate) const {
    const auto values = loc_candidates_.find(candidate.key_hash);
    if (values == loc_candidates_.end() || values->second.empty()) {
        LogAuthoritativeLocProblem(candidate.key_hash, 0u, "LOC_HASH_MISSING");
        return nullptr;
    }

    const auto& candidates = values->second;
    if (candidates.size() == 1u) return &candidates.front();
    for (const std::wstring& value : candidates) {
        if (value == candidate.replacement) return &value;
    }

    LogAuthoritativeLocProblem(candidate.key_hash, candidates.size(),
                               "MULTI_VALUE_CONTEXT_MISMATCH");
    return nullptr;
}

void ExternalLocOverrides::RememberPreciseMapping(
    const IndexedOverride& candidate) {
    PreciseThreadContext& context = PreciseContext();
    if (context.owner != this) context.Reset(this);
    const uint64_t sequence = ++context.next_sequence;
    RecentPreciseMapping& recent =
        context.recent[static_cast<size_t>(sequence & 63u)];
    recent.mapping = &candidate;
    recent.tick = GetTickCount();
    recent.sequence = sequence;
}

void ExternalLocOverrides::RememberGlobalDeferred(
    uint64_t event_id, uint32_t key_hash, bool explicit_defer,
    const std::wstring& source, const std::wstring& replacement) {
    if (source.empty() || replacement.empty()) return;
    std::lock_guard<std::mutex> lock(global_deferred_mutex_);
    const uint64_t sequence = ++global_deferred_sequence_;
    GlobalDeferredMapping& entry =
        global_deferred_[static_cast<size_t>(sequence & 63u)];
    entry.tick = GetTickCount();
    entry.thread_id = GetCurrentThreadId();
    entry.sequence = sequence;
    entry.event_id = event_id;
    entry.key_hash = key_hash;
    entry.explicit_defer = explicit_defer;
    entry.source = source;
    entry.replacement = replacement;
}

const wchar_t* ExternalLocOverrides::ApplyGlobalDeferred(
    const std::wstring& submitted, uint32_t* authoritative_source_units) {
    if (submitted.empty()) return nullptr;

    struct Candidate {
        std::wstring source;
        std::wstring replacement;
        size_t at = std::wstring::npos;
        uint64_t sequence = 0;
        uint64_t event_id = 0;
        uint32_t key_hash = 0;
        DWORD get_text_tick = 0;
        DWORD get_text_thread = 0;
        bool explicit_defer = false;
    };

    const DWORD now = GetTickCount();
    std::vector<Candidate> exact_matches;
    std::vector<Candidate> substring_matches;
    {
        std::lock_guard<std::mutex> lock(global_deferred_mutex_);
        const uint64_t newest = global_deferred_sequence_;
        for (uint64_t step = 0; step < 64u && step < newest; ++step) {
            const uint64_t sequence = newest - step;
            const GlobalDeferredMapping& entry =
                global_deferred_[static_cast<size_t>(sequence & 63u)];
            if (entry.sequence != sequence || entry.source.empty() ||
                entry.replacement.empty() ||
                now - entry.tick > kCrossThreadDeferredWindowMs) {
                continue;
            }

            if (submitted == entry.source) {
                exact_matches.push_back(
                    {entry.source, entry.replacement, 0u, sequence, entry.event_id, entry.key_hash, entry.tick, entry.thread_id, entry.explicit_defer});
                continue;
            }
            if (entry.source.size() < 4u) continue;
            const size_t at = FindStructuredOccurrence(submitted, entry.source);
            if (at != std::wstring::npos) {
                substring_matches.push_back(
                    {entry.source, entry.replacement, at, sequence, entry.event_id, entry.key_hash, entry.tick, entry.thread_id, entry.explicit_defer});
            }
        }
    }

    auto select_unique = [](const std::vector<Candidate>& candidates,
                            bool require_same_source, Candidate* selected) {
        if (!selected || candidates.empty()) return false;
        const Candidate* newest = nullptr;
        const std::wstring* source = nullptr;
        const std::wstring* replacement = nullptr;
        for (const Candidate& candidate : candidates) {
            if (!source) {
                source = &candidate.source;
                replacement = &candidate.replacement;
            } else {
                if (require_same_source && candidate.source != *source)
                    return false;
                if (candidate.replacement != *replacement) return false;
            }
            if (!newest || candidate.sequence > newest->sequence)
                newest = &candidate;
        }
        if (!newest) return false;
        *selected = *newest;
        return true;
    };

    Candidate selected;
    bool matched = false;
    if (!exact_matches.empty()) {
        // 一旦存在整串精确候选，就绝不退回别的子串候选。精确候选自身若
        // 因语境冲突不能达成一致，本次保持英语，等待更精确的上下文证据。
        matched = select_unique(exact_matches, false, &selected);
        if (!matched) return nullptr;
    } else {
        // 子串回退比整串精确命中更保守：必须只有同一个源文族，且所有
        // 候选 replacement 完全一致，才允许把该片段嵌回组合后的字幕。
        matched = select_unique(substring_matches, true, &selected);
        if (!matched) return nullptr;
    }

    CrossThreadOutputContext& context = CrossThreadContext();
    if (context.owner != this) context.Reset(this);
    context.output = submitted;
    context.output.replace(selected.at, selected.source.size(),
                           selected.replacement);
    if (authoritative_source_units) {
        *authoritative_source_units =
            static_cast<uint32_t>(selected.source.size());
    }

    bool expected = false;
    if (deferred_cross_thread_logged_.compare_exchange_strong(expected, true)) {
        BSCN_LOG_INFO(
            "SUBTITLE_DEFERRED_CROSS_THREAD_HIT policy=UNIQUE_AGREEMENT window_ms=%u source_units=%u",
            static_cast<unsigned>(kCrossThreadDeferredWindowMs),
            static_cast<unsigned>(selected.source.size()));
    }
    if (selected.explicit_defer) {
        BSCN_LOG_INFO(
            "CG_DEFER_SUBMIT event=%llu key_hash=0x%08X route=CROSS_THREAD get_thread=%u submit_thread=%u elapsed_ms=%u source_units=%u",
            static_cast<unsigned long long>(selected.event_id), selected.key_hash,
            static_cast<unsigned>(selected.get_text_thread),
            static_cast<unsigned>(GetCurrentThreadId()),
            static_cast<unsigned>(now-selected.get_text_tick),
            static_cast<unsigned>(selected.source.size()));
    }
    return context.output.c_str();
}

bool ExternalLocOverrides::ShouldAutoDeferToSubmitText(
    const std::wstring& source) {
    if (source.empty()) return false;

    // Dynamic format/token templates should remain English until SubmitText so
    // the game can perform its native formatting/layout first. This preserves
    // original key context without another late object hook.
    for (size_t i = 0; i < source.size(); ++i) {
        if (source[i] == L'%' && i + 1u < source.size()) {
            if (source[i + 1u] == L'%') { ++i; continue; }
            size_t cursor = i + 1u;
            while (cursor < source.size() && cursor - i <= 24u) {
                const wchar_t c = source[cursor];
                const bool conversion =
                    c == L'd' || c == L'i' || c == L'u' || c == L'o' ||
                    c == L'x' || c == L'X' || c == L'f' || c == L'F' ||
                    c == L'e' || c == L'E' || c == L'g' || c == L'G' ||
                    c == L'a' || c == L'A' || c == L'c' || c == L'C' ||
                    c == L's' || c == L'S' || c == L'p' || c == L'n';
                if (conversion) return true;
                const bool modifier =
                    (c >= L'0' && c <= L'9') || c == L'$' || c == L'-' ||
                    c == L'+' || c == L' ' || c == L'#' || c == L'.' ||
                    c == L'*' || c == L'h' || c == L'l' || c == L'j' ||
                    c == L'z' || c == L't' || c == L'L' || c == L'I';
                if (!modifier) break;
                ++cursor;
            }
        }
        if (i + 2u < source.size() && source[i] == L'~' &&
            source[i + 1u] == L'~' && source[i + 2u] == L'{') {
            return true;
        }
    }

    // R10: ordinary dialogue is NOT auto-deferred. Cinematic timing semantics
    // are driven only by the 221 explicit BLI3 DEFER_TO_SUBMIT_TEXT records.
    // Automatic defer remains limited to real dynamic printf/control-token
    // templates whose formatting genuinely happens after GetText.
    return false;
}

const wchar_t* ExternalLocOverrides::ResolveCandidate(
    const IndexedOverride& candidate, const wchar_t* original,
    uint32_t primary_hash, uint32_t secondary_hash) {
    const std::wstring* authoritative = FindAuthoritativeReplacement(candidate);
    if (!authoritative || authoritative->empty()) return nullptr;

    const size_t original_units =
        original && *original ? std::wcslen(original) : 0u;
    if (hud_bypass_dynamic_scalars_ && original_units != 0u &&
        IsDynamicRuntimeScalar(original, original_units)) {
        bool expected = false;
        if (dynamic_scalar_gettext_logged_.compare_exchange_strong(expected, true)) {
            BSCN_LOG_INFO(
                "HUD_DYNAMIC_SCALAR_GETTEXT_BYPASS route=GAME_OWNED_POINTER resolver=SKIPPED stable=SKIPPED");
        }
        return nullptr;
    }

    // 内容没有变化时绝不把游戏自己的字符串指针替换为 LOC 常驻指针。
    // 这是零可见差异的生命周期修复：MP7 -> MP7 等文本仍完全相同。
    if (original_units != 0u && authoritative->size() == original_units &&
        std::wmemcmp(original, authoritative->c_str(), original_units) == 0) {
        return nullptr;
    }

    RememberPreciseMapping(candidate);
    const bool explicit_defer =
        (candidate.flags & kIndexFlagDeferToSubmitText) != 0;
    const bool standard_auto_defer =
        ShouldAutoDeferToSubmitText(candidate.source);
    // 只在 R10 source-first 已确认“native source 正确，但当前 runtime hash
    // 明确不属于所选 BLI record”时改变交付时机。普通 source/hash 一致文本
    // 完全保持 R10.4 原有 GetText 中文返回语义，避免扩大回归面。
    const bool any_runtime_hash = primary_hash != 0u || secondary_hash != 0u;
    const bool selected_hash_matches =
        (primary_hash != 0u && candidate.key_hash == primary_hash) ||
        (secondary_hash != 0u && candidate.key_hash == secondary_hash);
    const bool source_first_hash_disagreement =
        any_runtime_hash && !selected_hash_matches;
    const bool hud_identity_defer =
        hud_defer_technical_labels_ && source_first_hash_disagreement &&
        original_units != 0u &&
        ShouldDeferTechnicalIdentityLabel(
            original, original_units, authoritative->c_str(), authoritative->size());
    const bool automatic_defer = standard_auto_defer || hud_identity_defer;
    if (!explicit_defer && !automatic_defer) return authoritative->c_str();

    if (standard_auto_defer && !explicit_defer) {
        bool expected = false;
        if (auto_defer_logged_.compare_exchange_strong(expected, true)) {
            BSCN_LOG_INFO(
                "R10_AUTO_DEFER_ACTIVE policy=PRINTF_TOKEN_ONLY timing=ORIGINAL_BEFORE_SUBMIT");
        }
    }
    if (hud_identity_defer) {
        bool log_it = false;
        {
            std::lock_guard<std::mutex> lock(hud_identity_log_mutex_);
            if (logged_hud_identity_defer_keys_.size() < 64u) {
                log_it = logged_hud_identity_defer_keys_.insert(candidate.key_hash).second;
            }
        }
        if (log_it) {
            BSCN_LOG_INFO(
                "HUD_IDENTITY_DEFER_GETTEXT key_hash=0x%08X primary=0x%08X secondary=0x%08X source=%s replacement=%s reason=SOURCE_FIRST_HASH_DISAGREEMENT delivery=SUBMIT_EXACT_CONTEXT game_pointer=PRESERVED",
                candidate.key_hash, primary_hash, secondary_hash,
                WideToUtf8(std::wstring(original, original_units)).c_str(),
                WideToUtf8(*authoritative).c_str());
        }
    }

    DeferredContext& context = ThreadContext();
    context.owner = this;
    context.tick = GetTickCount();
    context.thread_id = GetCurrentThreadId();
    context.event_id = ++deferred_event_sequence_;
    context.key_hash = candidate.key_hash;
    context.explicit_defer = explicit_defer;
    // Preserve the actual native string seen at GetText. This allows the game
    // to perform formatting/layout while the context metadata remains attached.
    context.source =
        original && *original ? std::wstring(original) : candidate.source;
    context.replacement = *authoritative;
    context.output.clear();
    RememberGlobalDeferred(context.event_id, context.key_hash,
                           context.explicit_defer, context.source,
                           context.replacement);
    if (explicit_defer) {
        BSCN_LOG_INFO(
            "CG_DEFER_GETTEXT event=%llu key_hash=0x%08X thread=%u tick=%u source_units=%u",
            static_cast<unsigned long long>(context.event_id), context.key_hash,
            static_cast<unsigned>(context.thread_id),
            static_cast<unsigned>(context.tick),
            static_cast<unsigned>(context.source.size()));
    }
    return nullptr;
}

const wchar_t* ExternalLocOverrides::Resolve(uint32_t primary_hash,
                                             uint32_t secondary_hash,
                                             const wchar_t* original) {
    if (!enabled_) return nullptr;
    if (!rendering_ready_.load()) {
        bool expected = false;
        if (font_gate_wait_logged_.compare_exchange_strong(expected, true)) {
            BSCN_LOG_INFO(
                "FONT_GATE_SKIP_GETTEXT action=KEEP_ORIGINAL until=ATLAS_READY");
        }
        return nullptr;
    }
    if (hud_bypass_dynamic_scalars_ && original && *original &&
        IsDynamicRuntimeScalar(original, std::wcslen(original))) {
        return nullptr;
    }

    // R10 context-first rule: native source identifies the text. Runtime hashes
    // are only disambiguators inside that exact source family. The latest real
    // log proved the same pair of runtime hashes can be observed for both MP7
    // and Silenced P99, so a hash can never establish identity by itself.
    if (const IndexedOverride* candidate =
            SelectSourceFirstCandidate(primary_hash, secondary_hash, original)) {
        return ResolveCandidate(*candidate, original, primary_hash, secondary_hash);
    }

    // Loose hash fallback is allowed only when BLI3 has no record for either
    // runtime hash and no exact source family could be selected. If a BLI3 hash
    // exists, staying English is safer than reviving hash-only guessing.
    const bool primary_indexed = primary_hash && by_hash_.find(primary_hash) != by_hash_.end();
    const bool secondary_indexed = secondary_hash && by_hash_.find(secondary_hash) != by_hash_.end();
    if (primary_indexed || secondary_indexed) return nullptr;
    const uint32_t hash = primary_hash ? primary_hash : secondary_hash;
    if (!hash) return nullptr;
    const auto loose = loc_overrides_.find(hash);
    return loose == loc_overrides_.end() ? nullptr : loose->second.c_str();
}

bool ExternalLocOverrides::ParseTemplateTokenAt(
    const std::wstring& text, size_t begin, size_t* token_end) {
    if (!token_end || begin >= text.size()) return false;
    size_t cursor = begin;
    bool wrapped = false;
    if (cursor + 2u < text.size() && text[cursor] == L'~' &&
        text[cursor + 1u] == L'~' && text[cursor + 2u] == L'{') {
        wrapped = true;
        cursor += 3u;
    } else if (text[cursor] == L'{') {
        ++cursor;
    } else {
        return false;
    }
    const size_t name_begin = cursor;
    while (cursor < text.size() && cursor - name_begin <= 64u) {
        if (text[cursor] == L'}') {
            if (cursor == name_begin) return false;
            ++cursor;
            if (wrapped) {
                if (cursor + 1u >= text.size() || text[cursor] != L'~' ||
                    text[cursor + 1u] != L'~') {
                    return false;
                }
                cursor += 2u;
            }
            *token_end = cursor;
            return true;
        }
        if (!IsAsciiTokenNameUnit(text[cursor])) return false;
        ++cursor;
    }
    return false;
}

bool ExternalLocOverrides::LooksLikeControlIsland(const std::wstring& capture) {
    size_t begin = 0;
    size_t end = capture.size();
    while (begin < end && IsAsciiSpace(capture[begin])) ++begin;
    while (end > begin && IsAsciiSpace(capture[end - 1u])) --end;
    if (begin == end || end - begin > 64u) return false;

    const std::wstring trimmed = capture.substr(begin, end - begin);
    for (size_t index = 0; index < trimmed.size(); ++index) {
        size_t token_end = 0;
        if (ParseTemplateTokenAt(trimmed, index, &token_end)) return true;
        if (static_cast<uint32_t>(trimmed[index]) >= 0x80u) return true;
    }

    // 某些路径会把图标暂时展开为很短的 ASCII 键名/组合，例如单个字母、
    // 数字或设备键名。这里只接受短串且必须出现大写字母或数字，避免普通
    // 英文句子中的单词被误当成“动态岛”。
    if (trimmed.size() > 24u) return false;
    bool saw_marker = false;
    for (wchar_t value : trimmed) {
        if ((value >= L'A' && value <= L'Z') ||
            (value >= L'0' && value <= L'9')) {
            saw_marker = true;
        }
        const bool allowed =
            (value >= L'A' && value <= L'Z') ||
            (value >= L'a' && value <= L'z') ||
            (value >= L'0' && value <= L'9') || value == L' ' ||
            value == L'+' || value == L'/' || value == L'|' ||
            value == L'&' || value == L'-' || value == L'(' ||
            value == L')';
        if (!allowed) return false;
    }
    return saw_marker;
}

bool ExternalLocOverrides::MatchDynamicTemplate(
    const std::wstring& input, const std::wstring& source,
    std::vector<std::pair<size_t, size_t>>* captures) {
    if (!captures) return false;
    std::vector<std::wstring> literals;
    size_t segment_begin = 0;
    size_t token_count = 0;
    for (size_t index = 0; index < source.size();) {
        size_t token_end = 0;
        if (!ParseTemplateTokenAt(source, index, &token_end)) {
            ++index;
            continue;
        }
        literals.push_back(source.substr(segment_begin, index - segment_begin));
        ++token_count;
        index = token_end;
        segment_begin = index;
    }
    if (token_count == 0u) return false;
    literals.push_back(source.substr(segment_begin));

    captures->clear();
    captures->reserve(token_count);
    size_t input_at = 0;
    if (!literals.front().empty()) {
        if (input.size() < literals.front().size() ||
            input.compare(0, literals.front().size(), literals.front()) != 0) {
            return false;
        }
        input_at = literals.front().size();
    }
    for (size_t token = 0; token < token_count; ++token) {
        const std::wstring& next_literal = literals[token + 1u];
        size_t capture_end = input.size();
        if (!next_literal.empty()) {
            capture_end = input.find(next_literal, input_at);
            if (capture_end == std::wstring::npos) return false;
        }
        if (capture_end < input_at) return false;
        const std::wstring capture =
            input.substr(input_at, capture_end - input_at);
        if (!LooksLikeControlIsland(capture)) return false;
        captures->push_back({input_at, capture_end});
        input_at = capture_end + next_literal.size();
    }
    return input_at == input.size();
}

bool ExternalLocOverrides::BuildDynamicReplacement(
    const std::wstring& input, const std::wstring& replacement,
    const std::vector<std::pair<size_t, size_t>>& captures,
    std::wstring* output) {
    if (!output) return false;
    output->clear();
    size_t capture_index = 0;
    size_t literal_begin = 0;
    for (size_t index = 0; index < replacement.size();) {
        size_t token_end = 0;
        if (!ParseTemplateTokenAt(replacement, index, &token_end)) {
            ++index;
            continue;
        }
        if (capture_index >= captures.size()) return false;
        output->append(replacement, literal_begin, index - literal_begin);
        const auto capture = captures[capture_index++];
        output->append(input, capture.first, capture.second - capture.first);
        index = token_end;
        literal_begin = index;
    }
    output->append(replacement, literal_begin,
                   replacement.size() - literal_begin);
    return capture_index == captures.size();
}


bool ExternalLocOverrides::ParsePrintfTemplate(
    const std::wstring& text, PrintfTemplateParts* parts) {
    if (!parts) return false;
    parts->literals.clear();
    parts->conversions.clear();

    auto is_conversion = [](wchar_t c) {
        switch (c) {
            case L'd': case L'i': case L'u': case L'o': case L'x': case L'X':
            case L'f': case L'F': case L'e': case L'E': case L'g': case L'G':
            case L'a': case L'A': case L'c': case L'C': case L's': case L'S':
            case L'p': case L'n':
                return true;
            default:
                return false;
        }
    };
    auto is_modifier = [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || c == L'$' || c == L'-' ||
               c == L'+' || c == L' ' || c == L'#' || c == L'.' ||
               c == L'*' || c == L'h' || c == L'l' || c == L'j' ||
               c == L'z' || c == L't' || c == L'L' || c == L'I';
    };

    std::wstring literal;
    for (size_t index = 0; index < text.size();) {
        if (text[index] != L'%') {
            literal.push_back(text[index++]);
            continue;
        }
        if (index + 1u < text.size() && text[index + 1u] == L'%') {
            literal.push_back(L'%');
            index += 2u;
            continue;
        }

        size_t end = index + 1u;
        while (end < text.size() && !is_conversion(text[end])) {
            if (!is_modifier(text[end])) return false;
            ++end;
        }
        if (end >= text.size() || !is_conversion(text[end])) return false;
        parts->literals.push_back(std::move(literal));
        literal.clear();
        parts->conversions.push_back(text[end]);
        index = end + 1u;
    }
    parts->literals.push_back(std::move(literal));
    return !parts->conversions.empty() &&
           parts->literals.size() == parts->conversions.size() + 1u;
}

bool ExternalLocOverrides::MatchPrintfTemplate(
    const std::wstring& input, const std::wstring& source,
    std::vector<std::wstring>* captures) {
    if (!captures) return false;
    PrintfTemplateParts parts;
    if (!ParsePrintfTemplate(source, &parts)) return false;

    captures->clear();
    captures->reserve(parts.conversions.size());
    size_t at = 0;
    if (!parts.literals.front().empty()) {
        const std::wstring& first = parts.literals.front();
        if (input.size() < first.size() || input.compare(0, first.size(), first) != 0)
            return false;
        at = first.size();
    }

    for (size_t index = 0; index < parts.conversions.size(); ++index) {
        const std::wstring& next = parts.literals[index + 1u];
        size_t end = input.size();
        if (!next.empty()) {
            end = input.find(next, at);
            if (end == std::wstring::npos) return false;
        } else if (index + 1u < parts.conversions.size()) {
            // 相邻占位符在最终字符串中没有可证明的边界，保持保守。
            return false;
        }
        captures->push_back(input.substr(at, end - at));
        at = end + next.size();
    }
    return at == input.size();
}

bool ExternalLocOverrides::BuildPrintfReplacement(
    const std::wstring& replacement,
    const std::vector<std::wstring>& captures, std::wstring* output) {
    if (!output) return false;
    PrintfTemplateParts parts;
    if (!ParsePrintfTemplate(replacement, &parts) ||
        parts.conversions.size() != captures.size()) {
        return false;
    }

    output->clear();
    output->append(parts.literals.front());
    for (size_t index = 0; index < captures.size(); ++index) {
        output->append(captures[index]);
        output->append(parts.literals[index + 1u]);
    }
    return true;
}

const wchar_t* ExternalLocOverrides::ApplyRecentPreciseMappings(
    const std::wstring& submitted) {
    PreciseThreadContext& context = PreciseContext();
    if (context.owner != this || context.next_sequence == 0u) return nullptr;

    const DWORD now = GetTickCount();
    const std::wstring* current = &submitted;
    std::wstring* destination = &context.output_a;
    bool changed = false;
    const IndexedOverride* used[32]{};
    size_t used_count = 0;
    const uint64_t newest = context.next_sequence;

    for (uint64_t step = 0; step < 64u && step < newest; ++step) {
        const uint64_t sequence = newest - step;
        const RecentPreciseMapping& recent =
            context.recent[static_cast<size_t>(sequence & 63u)];
        if (recent.sequence != sequence || !recent.mapping ||
            now - recent.tick > kRecentPreciseWindowMs) {
            continue;
        }
        bool duplicate = false;
        for (size_t index = 0; index < used_count; ++index) {
            if (used[index] == recent.mapping) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        if (used_count < sizeof(used) / sizeof(used[0])) {
            used[used_count++] = recent.mapping;
        }

        const IndexedOverride& mapping = *recent.mapping;
        const std::wstring authoritative =
            CopyAuthoritativeReplacement(mapping.key_hash, mapping.replacement);
        if (authoritative.empty()) continue;
        size_t at = std::wstring::npos;
        if (*current == mapping.source) {
            at = 0u;
        } else if (mapping.source.size() >= 4u) {
            // R8 禁止普通 substring replacement。只有冒号/已本地化 carrier
            // 等强结构字段才允许嵌回 recent precise mapping。
            at = FindStructuredOccurrence(*current, mapping.source);
        }
        if (at != std::wstring::npos) {
            *destination = *current;
            destination->replace(at, mapping.source.size(), authoritative);
        } else {
            bool capture_replaced = false;

            // R8：任务 carrier 和它的 %s 参数可能分别经过不同 GetText key。
            // 对当前精确 mapping，如果它的 source 恰好是“另一条最近精确
            // printf 模板”匹配出来的完整 capture，则允许只替换这个 capture。
            // 这是 key-context pair 的结构证明，不是 substring 搜索。
            std::wstring agreed_capture_output;
            bool capture_candidate_seen = false;
            bool capture_candidate_conflict = false;
            for (uint64_t container_step = 0;
                 container_step < 64u && container_step < newest;
                 ++container_step) {
                const uint64_t container_sequence = newest - container_step;
                const RecentPreciseMapping& container_recent =
                    context.recent[static_cast<size_t>(container_sequence & 63u)];
                if (container_recent.sequence != container_sequence ||
                    !container_recent.mapping ||
                    container_recent.mapping == &mapping ||
                    now - container_recent.tick > kRecentPreciseWindowMs) {
                    continue;
                }

                const IndexedOverride& container_mapping =
                    *container_recent.mapping;
                std::vector<std::wstring> container_captures;
                if (!MatchPrintfTemplate(*current, container_mapping.source,
                                         &container_captures) ||
                    container_captures.empty()) {
                    continue;
                }

                bool owns_capture = false;
                for (const std::wstring& capture : container_captures) {
                    if (capture == mapping.source) {
                        owns_capture = true;
                        break;
                    }
                }
                if (!owns_capture) continue;

                PrintfTemplateParts container_parts;
                if (!ParsePrintfTemplate(container_mapping.source,
                                         &container_parts) ||
                    container_parts.conversions.size() !=
                        container_captures.size()) {
                    continue;
                }
                std::wstring candidate;
                candidate.append(container_parts.literals.front());
                bool replaced_any_capture = false;
                for (size_t capture_index = 0;
                     capture_index < container_captures.size();
                     ++capture_index) {
                    if (container_captures[capture_index] == mapping.source) {
                        candidate.append(authoritative);
                        replaced_any_capture = true;
                    } else {
                        candidate.append(container_captures[capture_index]);
                    }
                    candidate.append(container_parts.literals[capture_index + 1u]);
                }
                if (!replaced_any_capture || candidate == *current) continue;

                if (!capture_candidate_seen) {
                    agreed_capture_output = std::move(candidate);
                    capture_candidate_seen = true;
                } else if (candidate != agreed_capture_output) {
                    capture_candidate_conflict = true;
                    break;
                }
            }
            if (capture_candidate_seen && !capture_candidate_conflict) {
                *destination = std::move(agreed_capture_output);
                capture_replaced = true;
                bool expected = false;
                if (precise_printf_logged_.compare_exchange_strong(
                        expected, true)) {
                    BSCN_LOG_INFO(
                        "TEXT_COVERAGE_HIT mode=recent_precise_printf_capture policy=ORIGINAL_KEY_CONTEXT_PAIR");
                }
            }

            if (!capture_replaced) {
                // 精确 key/hash 选中的 source 可能是 printf 模板；最终 SubmitText
                // 已经看不到字面 %s/%d。先按模板固定文字恢复运行时参数。由于
                // mapping 本身来自原始 key 语境，这不会对同源不同译文做全局猜测。
                std::vector<std::wstring> printf_captures;
                if (MatchPrintfTemplate(*current, mapping.source,
                                        &printf_captures) &&
                    BuildPrintfReplacement(authoritative, printf_captures,
                                           destination)) {
                    bool expected = false;
                    if (precise_printf_logged_.compare_exchange_strong(
                            expected, true)) {
                        BSCN_LOG_INFO(
                            "TEXT_COVERAGE_HIT mode=recent_precise_printf policy=ORIGINAL_KEY_CONTEXT_ONLY");
                    }
                } else {
                    std::vector<std::pair<size_t, size_t>> captures;
                    if (!MatchDynamicTemplate(*current, mapping.source,
                                              &captures) ||
                        !BuildDynamicReplacement(*current, authoritative,
                                                 captures, destination)) {
                        continue;
                    }
                }
            }
        }
        changed = true;
        current = destination;
        destination = destination == &context.output_a ? &context.output_b
                                                       : &context.output_a;
    }
    if (!changed) return nullptr;

    bool expected = false;
    if (precise_submit_logged_.compare_exchange_strong(expected, true)) {
        BSCN_LOG_INFO(
            "TEXT_COVERAGE_HIT mode=recent_precise_gettext policy=ORIGINAL_KEY_CONTEXT_ONLY");
    }
    return current->c_str();
}

const wchar_t* ExternalLocOverrides::ResolveSubmittedText(
    const wchar_t* submitted, uint32_t* authoritative_source_units) {
    if (authoritative_source_units) *authoritative_source_units = 0;
    if (!enabled_ || !submitted || !*submitted || !rendering_ready_.load())
        return nullptr;

    DeferredContext& deferred = ThreadContext();
    if (deferred.owner == this && !deferred.source.empty() &&
        !deferred.replacement.empty()) {
        if (GetTickCount() - deferred.tick <= kDeferredWindowMs) {
            const std::wstring current(submitted);
            const size_t at = FindStructuredOccurrence(current, deferred.source);
            if (at != std::wstring::npos) {
                deferred.output = current;
                deferred.output.replace(at, deferred.source.size(),
                                        deferred.replacement);
                if (authoritative_source_units) {
                    *authoritative_source_units =
                        static_cast<uint32_t>(deferred.source.size());
                }
                const wchar_t* result = deferred.output.c_str();
                const DWORD submit_tick = GetTickCount();
                const uint64_t event_id = deferred.event_id;
                const uint32_t key_hash = deferred.key_hash;
                const DWORD get_tick = deferred.tick;
                const DWORD get_thread = deferred.thread_id;
                const bool explicit_defer = deferred.explicit_defer;
                const size_t source_units = deferred.source.size();
                deferred.owner = nullptr;
                deferred.tick = 0;
                deferred.thread_id = 0;
                deferred.event_id = 0;
                deferred.key_hash = 0;
                deferred.explicit_defer = false;
                deferred.source.clear();
                deferred.replacement.clear();
                bool expected = false;
                if (deferred_same_thread_logged_.compare_exchange_strong(
                        expected, true)) {
                    BSCN_LOG_INFO(
                        "SUBTITLE_DEFERRED_SAME_THREAD_HIT policy=ORIGINAL_KEY_CONTEXT_ONLY");
                }
                if (explicit_defer) {
                    BSCN_LOG_INFO(
                        "CG_DEFER_SUBMIT event=%llu key_hash=0x%08X route=SAME_THREAD get_thread=%u submit_thread=%u elapsed_ms=%u source_units=%u",
                        static_cast<unsigned long long>(event_id), key_hash,
                        static_cast<unsigned>(get_thread),
                        static_cast<unsigned>(GetCurrentThreadId()),
                        static_cast<unsigned>(submit_tick-get_tick),
                        static_cast<unsigned>(source_units));
                }
                return result;
            }
        } else {
            deferred.Reset();
        }
    }

    const std::wstring current(submitted);
    if (const wchar_t* precise = ApplyRecentPreciseMappings(current))
        return precise;
    return ApplyGlobalDeferred(current, authoritative_source_units);
}

void ExternalLocOverrides::Shutdown() {
    enabled_ = false;
    rendering_ready_.store(true);
    root_.clear();
    index_path_.clear();
    language_[0] = L'e';
    language_[1] = L'n';
    loc_file_count_ = 0;
    valid_loc_file_count_ = 0;
    loc_overrides_.clear();
    loc_candidates_.clear();
    records_.clear();
    by_hash_.clear();
    by_source_.clear();
    ambiguous_miss_logged_.store(false);
    {
        std::lock_guard<std::mutex> lock(source_mismatch_log_mutex_);
        logged_source_mismatch_hashes_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(source_first_log_mutex_);
        logged_source_first_disagreements_.clear();
    }

    precise_submit_logged_.store(false);
    precise_printf_logged_.store(false);
    deferred_same_thread_logged_.store(false);
    deferred_cross_thread_logged_.store(false);
    auto_defer_logged_.store(false);
    {
        std::lock_guard<std::mutex> lock(hud_identity_log_mutex_);
        logged_hud_identity_defer_keys_.clear();
    }
    dynamic_scalar_gettext_logged_.store(false);
    hud_bypass_dynamic_scalars_ = true;
    hud_defer_technical_labels_ = true;
    font_gate_wait_logged_.store(false);
    {
        std::lock_guard<std::mutex> lock(authoritative_log_mutex_);
        logged_authoritative_problem_hashes_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(global_deferred_mutex_);
        global_deferred_sequence_ = 0;
        deferred_event_sequence_.store(0);
        for (GlobalDeferredMapping& entry : global_deferred_) entry = {};
    }
    DeferredContext& deferred = ThreadContext();
    if (deferred.owner == this) deferred.Reset();
    PreciseThreadContext& precise = PreciseContext();
    if (precise.owner == this) precise.Reset();
    CrossThreadOutputContext& cross_thread = CrossThreadContext();
    if (cross_thread.owner == this) cross_thread.Reset();
}

}  // namespace bscn
