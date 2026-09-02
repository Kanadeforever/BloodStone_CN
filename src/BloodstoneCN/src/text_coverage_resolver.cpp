#include "text_coverage_resolver.h"

#include "common.h"
#include "external_loc.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <utility>

namespace bscn {
namespace {
constexpr size_t kBliHeaderSize = 32;
constexpr uint32_t kBliVersion = 3;
constexpr uint32_t kMaxRecords = 200000;
constexpr uint32_t kMaxStringUnits = 8192;

uint32_t ReadU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8u) |
           (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}
uint16_t ReadU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8u);
}

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>* output) {
    if (!output) return false;
    output->clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    const bool size_ok = GetFileSizeEx(file, &size) != FALSE &&
                         size.QuadPart >= 0 &&
                         size.QuadPart <= 64ll * 1024ll * 1024ll;
    if (!size_ok) {
        CloseHandle(file);
        return false;
    }
    output->resize(static_cast<size_t>(size.QuadPart));
    size_t at = 0;
    bool ok = true;
    while (at < output->size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(
            output->size() - at, static_cast<size_t>(1u << 20)));
        DWORD read = 0;
        if (!ReadFile(file, output->data() + at, chunk, &read, nullptr) ||
            read != chunk) {
            ok = false;
            break;
        }
        at += read;
    }
    CloseHandle(file);
    if (!ok) output->clear();
    return ok;
}

std::wstring ReadWide(const uint8_t* data, size_t offset, uint32_t units) {
    std::wstring result;
    result.reserve(units);
    for (uint32_t index = 0; index < units; ++index) {
        result.push_back(static_cast<wchar_t>(
            ReadU16(data + offset + static_cast<size_t>(index) * 2u)));
    }
    return result;
}

bool IsTokenNameUnit(wchar_t c) {
    return c >= 0x21 && c <= 0x7E && c != L'{' && c != L'}' && c != L'~';
}
}  // namespace

bool TextCoverageResolver::Initialize(const std::wstring& bli_path,
                                      const ExternalLocOverrides* loc,
                                      bool log_action_tokens) {
    Shutdown();
    log_action_tokens_ = log_action_tokens;
    // R7 运行时不允许脱离 authoritative loose LOC 单独使用 BLI3。
    // BLI3 只负责 source/hash/context；最终 replacement 必须由 LOC 提供。
    if (!loc) return false;

    std::vector<uint8_t> file;
    if (!ReadWholeFile(bli_path, &file) || file.size() < kBliHeaderSize)
        return false;
    static constexpr char kMagic[8] = {'B','S','C','N','L','I','3','\0'};
    if (memcmp(file.data(), kMagic, sizeof(kMagic)) != 0 ||
        ReadU32(file.data() + 8u) != kBliVersion ||
        ReadU32(file.data() + 12u) != kBliHeaderSize) {
        return false;
    }
    const uint32_t count = ReadU32(file.data() + 16u);
    const uint32_t payload_size = ReadU32(file.data() + 20u);
    const uint32_t changed_count = ReadU32(file.data() + 24u);
    if (count == 0u || count > kMaxRecords || changed_count != count ||
        payload_size != file.size() - kBliHeaderSize) {
        return false;
    }

    std::vector<RawRecord> records;
    records.reserve(count);
    size_t position = kBliHeaderSize;
    for (uint32_t index = 0; index < count; ++index) {
        if (position + 16u > file.size()) return false;
        RawRecord record;
        record.key_hash = ReadU32(file.data() + position);
        const uint32_t source_units = ReadU32(file.data() + position + 4u);
        const uint32_t replacement_units =
            ReadU32(file.data() + position + 8u);
        record.flags = ReadU32(file.data() + position + 12u);
        position += 16u;
        if (source_units == 0u || replacement_units == 0u ||
            source_units > kMaxStringUnits ||
            replacement_units > kMaxStringUnits) {
            return false;
        }
        const uint64_t bytes =
            static_cast<uint64_t>(source_units + replacement_units) * 2u;
        if (bytes > file.size() - position) return false;
        record.source = ReadWide(file.data(), position, source_units);
        position += static_cast<size_t>(source_units) * 2u;
        record.indexed_replacement =
            ReadWide(file.data(), position, replacement_units);
        position += static_cast<size_t>(replacement_units) * 2u;

        // BLI3 仅提供 source/hash/context。真正 replacement 必须回到当前
        // 86 个 loose LOC。indexed_replacement 只作为“同 hash 多候选”时
        // 识别历史语境的指纹；它绝不直接成为最终显示正文。
        const std::wstring authoritative = loc->CopyAuthoritativeReplacement(
            record.key_hash, record.indexed_replacement);
        if (authoritative.empty()) {
            ++stats_.skipped_ambiguous_loc_sources;
            record.replacement.clear();
        } else {
            if (authoritative != record.indexed_replacement)
                ++stats_.authoritative_loc_sources;
            record.replacement = authoritative;
        }
        records.push_back(std::move(record));
    }
    if (position != file.size()) return false;

    // 先建立严格唯一 source 映射：同一英语源文只要出现两个不同译文就排除。
    std::unordered_set<std::wstring> ambiguous_exact;
    exact_.reserve(records.size());
    for (const RawRecord& record : records) {
        if (record.source.empty() || record.replacement.empty() ||
            ambiguous_exact.find(record.source) != ambiguous_exact.end()) {
            continue;
        }
        if (!IsSafeSourceFallbackReplacement(record.source,
                                             record.replacement)) {
            if (IsMeaningfulReplacement(record.source, record.replacement))
                ++stats_.rejected_partial_english_sources;
            continue;
        }
        const auto found = exact_.find(record.source);
        if (found == exact_.end()) {
            exact_.emplace(record.source, record.replacement);
        } else if (found->second != record.replacement) {
            exact_.erase(found);
            ambiguous_exact.insert(record.source);
        }
    }
    stats_.ambiguous_sources = ambiguous_exact.size();

    // 对极短的 ASCII 枚举，仅允许一种完全由数据本身证明的别名关系：
    // 同一 source 恰好有两个不同 replacement，且一个是另一个的严格前缀。
    // 选择已有数据中的较长项，不写死 source，也不写死任何中文正文。
    BuildShortPrefixAliases(records);

    // 城市/地点等短标签在 LOC 中可能同时存在“基础名称”和“基础名称+说明”
    // 两类记录。只有当所有候选都由同一个、且本身真实存在于 LOC 的基础候选
    // 前缀扩展而来时，才把这个基础候选恢复为安全 source 别名。没有任何
    // source 名称或译文硬编码。
    BuildBaseLabelAliases(records);

    // 说话者冒号前后空格等运行时差异建立 canonical 索引。
    BuildCanonicalAliases(records);

    // 规范化索引从全部权威 LOC/BLI3 记录建立，而不是只从 exact_ 建立。
    // 因此尾随空格等运行时差异仍可恢复；规范化后出现不同译文则整体排除。
    BuildNormalizedAliases(records);

    printf_rules_.clear();
    tutorial_rules_.clear();
    for (const auto& item : exact_) {
        TemplateRule printf_rule;
        printf_rule.source = item.first;
        size_t source_count = 0;
        size_t replacement_count = 0;
        if (ParsePrintfTemplate(item.first, &printf_rule.source_parts,
                                &source_count) &&
            ParsePrintfTemplate(item.second, &printf_rule.replacement_parts,
                                &replacement_count) &&
            source_count != 0u && source_count == replacement_count) {
            printf_rule.placeholder_count = source_count;
            for (const FormatPart& part : printf_rule.source_parts) {
                if (!part.placeholder) printf_rule.literal_units += part.literal.size();
            }
            printf_rules_.push_back(std::move(printf_rule));
        }

        TemplateRule token_rule;
        token_rule.source = item.first;
        source_count = 0;
        replacement_count = 0;
        if (ParseTokenTemplate(item.first, &token_rule.source_parts,
                               &source_count) &&
            ParseTokenTemplate(item.second, &token_rule.replacement_parts,
                               &replacement_count) &&
            source_count != 0u && source_count == replacement_count) {
            token_rule.placeholder_count = source_count;
            for (const FormatPart& part : token_rule.source_parts) {
                if (!part.placeholder) token_rule.literal_units += part.literal.size();
            }
            tutorial_rules_.push_back(std::move(token_rule));
        }
    }
    auto specificity = [](const TemplateRule& left,
                          const TemplateRule& right) {
        return left.literal_units > right.literal_units;
    };
    // exact_ 会排除同 source 多 key 的项目；任务状态模板恰好经常属于这种
    // 结构。再从全部 BLI3 记录建立“同 source 模板且 replacement 完全一致”
    // 的安全 printf 规则。仍然不按第一候选猜测。
    BuildSafeAllRecordPrintfRules(records);
    // R8：同 source 模板在不同 key 下可能有多个 carrier 译法。不能取第一条，
    // 但可以在“具体运行时字符串”层对全部候选求一致结果。
    BuildAllRecordPrintfCandidates(records);

    std::sort(printf_rules_.begin(), printf_rules_.end(), specificity);
    std::sort(all_printf_rules_.begin(), all_printf_rules_.end(), specificity);
    std::sort(tutorial_rules_.begin(), tutorial_rules_.end(), specificity);

    BuildSubtitleBodyIndex(records);

    stats_.records = records.size();
    stats_.unique_sources = exact_.size();
    stats_.unique_normalized_sources = normalized_.size();
    stats_.printf_templates = printf_rules_.size();
    stats_.tutorial_templates = tutorial_rules_.size();
    stats_.subtitle_bodies = subtitle_bodies_.size();
    ready_ = true;
    return true;
}

void TextCoverageResolver::BuildShortPrefixAliases(
    const std::vector<RawRecord>& records) {
    std::unordered_map<std::wstring, std::vector<std::wstring>> grouped;
    for (const RawRecord& record : records) {
        if (record.replacement.empty() ||
            !IsMeaningfulReplacement(record.source, record.replacement) ||
            record.source.size() > 4u || !IsAsciiAlphaString(record.source))
            continue;
        auto& values = grouped[record.source];
        if (std::find(values.begin(), values.end(), record.replacement) ==
            values.end()) {
            values.push_back(record.replacement);
        }
    }
    for (const auto& item : grouped) {
        const auto& values = item.second;
        if (values.size() != 2u || values[0].empty() || values[1].empty())
            continue;
        const std::wstring* shorter = &values[0];
        const std::wstring* longer = &values[1];
        if (shorter->size() > longer->size()) std::swap(shorter, longer);
        if (shorter->size() == longer->size() ||
            longer->compare(0, shorter->size(), *shorter) != 0) {
            continue;
        }
        exact_[item.first] = *longer;
        ++stats_.prefix_alias_sources;
    }
}

void TextCoverageResolver::BuildBaseLabelAliases(
    const std::vector<RawRecord>& records) {
    std::unordered_map<std::wstring, std::vector<std::wstring>> grouped;
    for (const RawRecord& record : records) {
        if (record.replacement.empty() ||
            !IsSafeSourceFallbackReplacement(record.source, record.replacement))
            continue;
        // 极短 ASCII 枚举已经由 BuildShortPrefixAliases 处理；这里仅处理
        // 单词形态的较长基础标签，降低把普通短 UI 枚举误归并的风险。
        if (record.source.size() <= 4u || !IsAsciiAlphaString(record.source))
            continue;
        auto& values = grouped[record.source];
        if (std::find(values.begin(), values.end(), record.replacement) ==
            values.end()) {
            values.push_back(record.replacement);
        }
    }

    for (const auto& item : grouped) {
        if (exact_.find(item.first) != exact_.end()) continue;
        const auto& values = item.second;
        if (values.size() < 2u) continue;

        const std::wstring* selected = nullptr;
        size_t valid_bases = 0;
        for (const std::wstring& candidate : values) {
            if (candidate.empty()) continue;
            bool is_base = true;
            bool has_extension = false;
            for (const std::wstring& other : values) {
                if (other == candidate) continue;
                if (other.size() <= candidate.size() ||
                    other.compare(0, candidate.size(), candidate) != 0) {
                    is_base = false;
                    break;
                }
                has_extension = true;
            }
            if (is_base && has_extension) {
                selected = &candidate;
                ++valid_bases;
            }
        }
        if (valid_bases != 1u || !selected) continue;
        exact_[item.first] = *selected;
        ++stats_.base_label_alias_sources;
    }
}

void TextCoverageResolver::BuildNormalizedAliases(
    const std::vector<RawRecord>& records) {
    normalized_.clear();
    std::unordered_set<std::wstring> ambiguous;
    normalized_.reserve(records.size());

    auto add = [&](const std::wstring& source, const std::wstring& replacement) {
        if (source.empty() || replacement.empty() ||
            !IsSafeSourceFallbackReplacement(source, replacement)) return;
        const std::wstring key =
            NormalizeWhitespace(source.c_str(), source.size());
        if (key.empty() || ambiguous.find(key) != ambiguous.end()) return;
        const auto found = normalized_.find(key);
        if (found == normalized_.end()) {
            normalized_.emplace(key, replacement);
        } else if (found->second != replacement) {
            normalized_.erase(found);
            ambiguous.insert(key);
        }
    };

    // 先用全部权威记录建立碰撞事实。这里若同一规范化源文存在不同译文，
    // 必须保持排除，后续任何别名都不能绕过这个冲突。
    for (const RawRecord& record : records) add(record.source, record.replacement);

    // 再补入已由数据关系证明安全的 exact 别名；但绝不复活已经冲突的 key。
    for (const auto& item : exact_) add(item.first, item.second);
    stats_.ambiguous_normalized_sources = ambiguous.size();
}


void TextCoverageResolver::BuildCanonicalAliases(
    const std::vector<RawRecord>& records) {
    canonical_.clear();
    std::unordered_set<std::wstring> ambiguous;
    canonical_.reserve(records.size());

    auto add = [&](const std::wstring& source, const std::wstring& replacement) {
        if (source.empty() || replacement.empty() ||
            !IsSafeSourceFallbackReplacement(source, replacement)) return;
        const std::wstring key =
            CanonicalizeLookupSource(source.c_str(), source.size());
        if (key.empty() || ambiguous.find(key) != ambiguous.end()) return;
        const auto found = canonical_.find(key);
        if (found == canonical_.end()) {
            canonical_.emplace(key, replacement);
        } else if (found->second != replacement) {
            canonical_.erase(found);
            ambiguous.insert(key);
        }
    };

    for (const RawRecord& record : records) add(record.source, record.replacement);
    for (const auto& item : exact_) add(item.first, item.second);
    stats_.canonical_sources = canonical_.size();
    stats_.ambiguous_canonical_sources = ambiguous.size();
}

void TextCoverageResolver::BuildSafeAllRecordPrintfRules(
    const std::vector<RawRecord>& records) {
    struct Candidate {
        std::wstring replacement;
        bool ambiguous = false;
    };
    std::unordered_map<std::wstring, Candidate> grouped;
    grouped.reserve(records.size());

    for (const RawRecord& record : records) {
        if (record.replacement.empty() ||
            !IsSafeSourceFallbackReplacement(record.source, record.replacement))
            continue;
        std::vector<FormatPart> source_parts;
        std::vector<FormatPart> replacement_parts;
        size_t source_count = 0;
        size_t replacement_count = 0;
        if (!ParsePrintfTemplate(record.source, &source_parts, &source_count) ||
            !ParsePrintfTemplate(record.replacement, &replacement_parts,
                                 &replacement_count) ||
            source_count == 0u || source_count != replacement_count) {
            continue;
        }
        Candidate& candidate = grouped[record.source];
        if (candidate.replacement.empty()) {
            candidate.replacement = record.replacement;
        } else if (candidate.replacement != record.replacement) {
            candidate.ambiguous = true;
        }
    }

    std::unordered_set<std::wstring> already;
    already.reserve(printf_rules_.size());
    for (const TemplateRule& rule : printf_rules_) already.insert(rule.source);

    for (const auto& item : grouped) {
        if (item.second.ambiguous || item.second.replacement.empty() ||
            already.find(item.first) != already.end()) {
            continue;
        }
        TemplateRule rule;
        rule.source = item.first;
        size_t source_count = 0;
        size_t replacement_count = 0;
        if (!ParsePrintfTemplate(item.first, &rule.source_parts, &source_count) ||
            !ParsePrintfTemplate(item.second.replacement,
                                 &rule.replacement_parts,
                                 &replacement_count) ||
            source_count == 0u || source_count != replacement_count) {
            continue;
        }
        rule.placeholder_count = source_count;
        for (const FormatPart& part : rule.source_parts) {
            if (!part.placeholder) rule.literal_units += part.literal.size();
        }
        printf_rules_.push_back(std::move(rule));
        ++stats_.safe_all_record_printf_templates;
    }
}

void TextCoverageResolver::BuildAllRecordPrintfCandidates(
    const std::vector<RawRecord>& records) {
    all_printf_rules_.clear();
    all_printf_rules_.reserve(records.size() / 8u);
    std::unordered_set<std::wstring> seen;

    for (const RawRecord& record : records) {
        if (record.replacement.empty() ||
            !IsSafeSourceFallbackReplacement(record.source,
                                             record.replacement)) {
            continue;
        }
        TemplateRule rule;
        rule.source = record.source;
        size_t source_count = 0;
        size_t replacement_count = 0;
        if (!ParsePrintfTemplate(record.source, &rule.source_parts,
                                 &source_count) ||
            !ParsePrintfTemplate(record.replacement, &rule.replacement_parts,
                                 &replacement_count) ||
            source_count == 0u || source_count != replacement_count) {
            continue;
        }
        // 同一 source/replacement 对可能由多个 key 重复出现；只去掉真正重复，
        // 不丢弃同 source 下的不同权威 replacement 候选。
        std::wstring fingerprint = record.source;
        fingerprint.push_back(static_cast<wchar_t>(0x1Fu));
        fingerprint.append(record.replacement);
        if (!seen.insert(std::move(fingerprint)).second) continue;

        rule.placeholder_count = source_count;
        for (const FormatPart& part : rule.source_parts) {
            if (!part.placeholder) rule.literal_units += part.literal.size();
        }
        all_printf_rules_.push_back(std::move(rule));
    }
    stats_.all_record_printf_candidates = all_printf_rules_.size();
}

void TextCoverageResolver::BuildSubtitleBodyIndex(
    const std::vector<RawRecord>& records) {
    subtitle_bodies_.clear();
    speaker_names_.clear();
    std::unordered_set<std::wstring> ambiguous_bodies;
    std::unordered_set<std::wstring> ambiguous_speakers;

    auto add_body = [&](const std::wstring& source,
                        const std::wstring& replacement) {
        if (source.empty() || replacement.empty() ||
            !IsSafeSourceFallbackReplacement(source, replacement)) return;
        const std::wstring key =
            NormalizeWhitespace(source.c_str(), source.size());
        if (key.empty() || ambiguous_bodies.find(key) != ambiguous_bodies.end())
            return;
        const auto found = subtitle_bodies_.find(key);
        if (found == subtitle_bodies_.end()) {
            subtitle_bodies_.emplace(key, replacement);
        } else if (found->second != replacement) {
            subtitle_bodies_.erase(found);
            ambiguous_bodies.insert(key);
        }
    };

    auto add_speaker = [&](const std::wstring& source,
                           const std::wstring& replacement) {
        const std::wstring source_key =
            NormalizeWhitespace(source.c_str(), source.size());
        const std::wstring replacement_key =
            NormalizeWhitespace(replacement.c_str(), replacement.size());
        if (source_key.empty() || replacement_key.empty() ||
            ambiguous_speakers.find(source_key) != ambiguous_speakers.end()) {
            return;
        }
        // M/Q 一类官方本来就不翻译的稳定标识符允许同值；其他 speaker 映射
        // 必须确实发生变化，且不能是“长英语几乎原样 + 一个 CJK”的伪覆盖。
        if (source_key != replacement_key &&
            !IsSafeSourceFallbackReplacement(source_key, replacement_key)) {
            return;
        }
        const auto found = speaker_names_.find(source_key);
        if (found == speaker_names_.end()) {
            speaker_names_.emplace(source_key, replacement_key);
        } else if (found->second != replacement_key) {
            speaker_names_.erase(found);
            ambiguous_speakers.insert(source_key);
        }
    };

    // 独立正文记录本身仍然是最重要的 body 来源。
    for (const auto& item : exact_) add_body(item.first, item.second);

    // 再从完整 Speaker: body 记录拆出正文和说话者。R8 的 SplitSpeakerBody
    // 同时支持 ASCII ':' 与中文全角 '：'，因此不会再因为 replacement 使用
    // 全角冒号而漏掉完整字幕里的正文。
    for (const RawRecord& record : records) {
        size_t source_separator = 0;
        size_t source_body = 0;
        size_t replacement_separator = 0;
        size_t replacement_body = 0;
        if (!SplitSpeakerBody(record.source, &source_separator, &source_body) ||
            !SplitSpeakerBody(record.replacement, &replacement_separator,
                              &replacement_body)) {
            continue;
        }
        add_body(record.source.substr(source_body),
                 record.replacement.substr(replacement_body));
        add_speaker(record.source.substr(0u, source_separator),
                    record.replacement.substr(0u, replacement_separator));
    }
    stats_.ambiguous_subtitle_bodies = ambiguous_bodies.size();
    stats_.speaker_aliases = speaker_names_.size();
    stats_.ambiguous_speaker_aliases = ambiguous_speakers.size();
}

void TextCoverageResolver::Shutdown() {
    ready_ = false;
    stats_ = Stats{};
    exact_.clear();
    normalized_.clear();
    canonical_.clear();
    subtitle_bodies_.clear();
    speaker_names_.clear();
    printf_rules_.clear();
    all_printf_rules_.clear();
    tutorial_rules_.clear();
    std::lock_guard<std::mutex> lock(observed_token_mutex_);
    observed_action_tokens_.clear();
}

TextCoverageResolver::Result TextCoverageResolver::Resolve(
    const wchar_t* submitted, std::wstring* scratch) const {
    if (!ready_ || !submitted || !*submitted || !scratch) return {};
    size_t units = 0;
    while (submitted[units] != L'\0' && units <= 16384u) ++units;
    if (units == 0u || units > 16384u) return {};

    // W1：纯动态 HUD 数值不是翻译对象。SubmitText 已在进入 resolver 前
    // 旁路；这里再加一层防御，保证其他内部调用也不会把弹药/计时等
    // game-owned buffer 放进 exact/canonical/template 解析链。
    if (IsDynamicRuntimeScalar(submitted, units)) return {};

    ObserveWrappedTokens(submitted, units);

    Result result = ResolveExact(submitted, units);
    if (result.text) return result;
    result = ResolveCanonical(submitted, units);
    if (result.text) return result;
    result = ResolvePrintf(submitted, units, scratch);
    if (result.text) return result;
    result = ResolvePrintfAgreement(submitted, units, scratch);
    if (result.text) return result;
    result = ResolveCompound(submitted, units, scratch);
    if (result.text) return result;
    result = ResolveActionPrompt(submitted, units, scratch);
    if (result.text) return result;
    result = ResolveTutorial(submitted, units, scratch);
    if (result.text) return result;
    result = ResolveSpeaker(submitted, units, scratch);
    if (result.text) return result;
    result = ResolveMixedLocalizedField(submitted, units, scratch);
    if (result.text) return result;
    return ResolveNormalized(submitted, units);
}

TextCoverageResolver::Result TextCoverageResolver::ResolveExact(
    const wchar_t* text, size_t units) const {
    const auto found = exact_.find(std::wstring(text, units));
    if (found == exact_.end()) return {};
    return {found->second.c_str(), static_cast<uint32_t>(units), Mode::Exact};
}

TextCoverageResolver::Result TextCoverageResolver::ResolveNormalized(
    const wchar_t* text, size_t units) const {
    const std::wstring key = NormalizeWhitespace(text, units);
    const auto found = normalized_.find(key);
    if (found == normalized_.end()) return {};
    return {found->second.c_str(), static_cast<uint32_t>(units),
            Mode::WhitespaceNormalized};
}

TextCoverageResolver::Result TextCoverageResolver::ResolveCanonical(
    const wchar_t* text, size_t units) const {
    const std::wstring key = CanonicalizeLookupSource(text, units);
    const auto found = canonical_.find(key);
    if (found == canonical_.end()) return {};
    return {found->second.c_str(), static_cast<uint32_t>(units),
            Mode::CanonicalSource};
}

TextCoverageResolver::Result TextCoverageResolver::ResolvePrintf(
    const wchar_t* text, size_t units, std::wstring* scratch) const {
    std::vector<std::wstring> captures;
    for (const TemplateRule& rule : printf_rules_) {
        captures.clear();
        if (!MatchTemplate(rule.source_parts, text, units, &captures) ||
            captures.size() != rule.placeholder_count) {
            continue;
        }
        if (!ComposePrintf(rule, captures, scratch)) continue;
        return {scratch->c_str(), static_cast<uint32_t>(units),
                Mode::PrintfTemplate};
    }
    return {};
}

TextCoverageResolver::Result TextCoverageResolver::ResolvePrintfAgreement(
    const wchar_t* text, size_t units, std::wstring* scratch) const {
    if (!scratch) return {};
    std::vector<std::wstring> captures;
    std::wstring candidate_output;
    std::wstring agreed_output;
    bool matched_any = false;

    for (const TemplateRule& rule : all_printf_rules_) {
        // 至少需要一段固定 literal；纯 %s 一类模板没有足够识别力。
        if (rule.literal_units < 3u) continue;
        captures.clear();
        if (!MatchTemplate(rule.source_parts, text, units, &captures) ||
            captures.size() != rule.placeholder_count) {
            continue;
        }
        candidate_output.clear();
        if (!ComposePrintf(rule, captures, &candidate_output) ||
            candidate_output.empty()) {
            continue;
        }
        if (!matched_any) {
            agreed_output = candidate_output;
            matched_any = true;
        } else if (candidate_output != agreed_output) {
            // 具体运行时串仍对应多个不同 LOC 结果时保持 NO_GUESS，等待
            // original key/hash 的 recent precise context 来消歧。
            return {};
        }
    }
    if (!matched_any) return {};
    *scratch = std::move(agreed_output);
    return {scratch->c_str(), static_cast<uint32_t>(units),
            Mode::PrintfAgreement};
}

bool TextCoverageResolver::ComposeCompoundSegments(
    const wchar_t* text, size_t units, std::wstring* output) const {
    if (!text || !output || units < 5u) return false;

    struct Delimiter {
        const wchar_t* text;
        size_t units;
    };
    // 只接受游戏实际使用的、带空格的可见分隔符。分隔符本身逐 UTF-16
    // 单元原样保留，不会重排按键 Token 或改变标点风格。
    static constexpr wchar_t kSlash[] = {L' ', L'/', L' ', L'\0'};
    static constexpr wchar_t kDash[] = {L' ', L'-', L' ', L'\0'};
    static constexpr Delimiter kDelimiters[] = {
        {kSlash, 3u}, {kDash, 3u},
    };

    auto resolve_segment = [&](const wchar_t* begin, size_t length,
                               std::wstring* replacement) -> bool {
        if (!replacement) return false;
        size_t left = 0;
        size_t right = length;
        while (left < right && IsSpace(begin[left])) ++left;
        while (right > left && IsSpace(begin[right - 1u])) --right;
        if (left == right) return false;

        Result resolved = ResolveExact(begin + left, right - left);
        if (!resolved.text)
            resolved = ResolveCanonical(begin + left, right - left);
        if (!resolved.text)
            resolved = ResolveNormalized(begin + left, right - left);
        if (!resolved.text) return false;

        replacement->assign(begin, left);
        replacement->append(resolved.text);
        replacement->append(begin + right, length - right);
        return true;
    };

    output->clear();
    size_t segment_begin = 0;
    size_t segment_count = 0;
    for (size_t cursor = 0; cursor < units;) {
        const Delimiter* matched = nullptr;
        for (const Delimiter& delimiter : kDelimiters) {
            if (cursor + delimiter.units <= units &&
                wmemcmp(text + cursor, delimiter.text, delimiter.units) == 0) {
                matched = &delimiter;
                break;
            }
        }
        if (!matched) {
            ++cursor;
            continue;
        }

        std::wstring replacement;
        if (!resolve_segment(text + segment_begin, cursor - segment_begin,
                             &replacement)) {
            output->clear();
            return false;
        }
        output->append(replacement);
        output->append(text + cursor, matched->units);
        ++segment_count;
        cursor += matched->units;
        segment_begin = cursor;
    }
    if (segment_count == 0u) {
        output->clear();
        return false;
    }

    std::wstring tail;
    if (!resolve_segment(text + segment_begin, units - segment_begin, &tail)) {
        output->clear();
        return false;
    }
    output->append(tail);
    return true;
}

TextCoverageResolver::Result TextCoverageResolver::ResolveCompound(
    const wchar_t* text, size_t units, std::wstring* scratch) const {
    if (!ComposeCompoundSegments(text, units, scratch)) return {};
    return {scratch->c_str(), static_cast<uint32_t>(units),
            Mode::CompoundSegments};
}

TextCoverageResolver::Result TextCoverageResolver::ResolveActionPrompt(
    const wchar_t* text, size_t units, std::wstring* scratch) const {
    size_t body_begin = 0;
    size_t body_end = units;
    if (!SplitWrappedControlTokenBody(text, units, &body_begin, &body_end) ||
        body_begin >= body_end) {
        return {};
    }
    Result body = ResolveExact(text + body_begin, body_end - body_begin);
    if (!body.text)
        body = ResolveCanonical(text + body_begin, body_end - body_begin);
    if (!body.text)
        body = ResolveNormalized(text + body_begin, body_end - body_begin);

    std::wstring compound_body;
    const wchar_t* replacement = body.text;
    if (!replacement &&
        ComposeCompoundSegments(text + body_begin, body_end - body_begin,
                                &compound_body)) {
        replacement = compound_body.c_str();
    }
    if (!replacement) return {};

    scratch->assign(text, body_begin);
    scratch->append(replacement);
    scratch->append(text + body_end, units - body_end);
    return {scratch->c_str(), static_cast<uint32_t>(units),
            Mode::ActionPromptBody};
}

TextCoverageResolver::Result TextCoverageResolver::ResolveTutorial(
    const wchar_t* text, size_t units, std::wstring* scratch) const {
    std::vector<std::wstring> captures;
    for (const TemplateRule& rule : tutorial_rules_) {
        captures.clear();
        if (!MatchTemplate(rule.source_parts, text, units, &captures) ||
            captures.size() != rule.placeholder_count) {
            continue;
        }
        bool safe = true;
        for (const std::wstring& capture : captures) {
            if (!LooksLikeControlIsland(capture)) {
                safe = false;
                break;
            }
        }
        if (!safe) continue;
        if (!ComposeTokenTemplate(rule.replacement_parts, captures, scratch))
            continue;
        return {scratch->c_str(), static_cast<uint32_t>(units),
                Mode::TutorialDynamicTokens};
    }
    return {};
}

TextCoverageResolver::Result TextCoverageResolver::ResolveSpeaker(
    const wchar_t* text, size_t units, std::wstring* scratch) const {
    const std::wstring current(text, units);
    size_t separator = 0;
    size_t body_begin = 0;
    if (!SplitSpeakerBody(current, &separator, &body_begin)) return {};

    const std::wstring raw_speaker(text, separator);
    const std::wstring speaker_key = NormalizeWhitespace(
        raw_speaker.c_str(), raw_speaker.size());
    const wchar_t* speaker_replacement = nullptr;

    Result speaker = ResolveExact(text, separator);
    if (!speaker.text) speaker = ResolveNormalized(text, separator);
    if (speaker.text) {
        speaker_replacement = speaker.text;
    } else {
        const auto derived = speaker_names_.find(speaker_key);
        if (derived != speaker_names_.end()) {
            speaker_replacement = derived->second.c_str();
        } else if (IsNeutralSpeakerIdentifier(speaker_key)) {
            // 例如官方角色键 CMN_CHARACTER_M 的 source/replacement 都是 M。
            // 这类稳定标识符不属于残留英语，不应阻止正文完整中文化。
            speaker_replacement = raw_speaker.c_str();
        }
    }

    const std::wstring body_key = NormalizeWhitespace(
        text + body_begin, units - body_begin);
    const auto derived_body = subtitle_bodies_.find(body_key);
    const std::wstring current_body(text + body_begin, units - body_begin);
    Result direct_body = ResolveExact(text + body_begin, units - body_begin);
    if (!direct_body.text)
        direct_body = ResolveCanonical(text + body_begin, units - body_begin);
    if (!direct_body.text)
        direct_body = ResolveNormalized(text + body_begin, units - body_begin);

    // Body must be fully deterministic, or already localized with only a neutral
    // speaker left. No substring completion is permitted.
    const bool body_already_localized=IsAlreadyLocalizedBody(current_body);
    if (!direct_body.text && derived_body==subtitle_bodies_.end() &&
        !body_already_localized) return {};
    if (!speaker_replacement) return {};

    scratch->clear();
    scratch->append(speaker_replacement);
    scratch->append(text + separator, body_begin - separator);
    if (direct_body.text)
        scratch->append(direct_body.text);
    else if (derived_body != subtitle_bodies_.end())
        scratch->append(derived_body->second);
    else
        scratch->append(current_body);
    return {scratch->c_str(), static_cast<uint32_t>(units),
            Mode::SpeakerAndBody};
}

TextCoverageResolver::Result TextCoverageResolver::ResolveMixedLocalizedField(
    const wchar_t* text, size_t units, std::wstring* scratch) const {
    if (!text || !scratch || units == 0u) return {};
    const std::wstring current(text, units);
    if (!HasCjkSignal(current)) return {};

    size_t tail_begin = 0;
    if (!FindStructuredTail(current, &tail_begin) || tail_begin >= units)
        return {};
    const std::wstring tail(text + tail_begin, units - tail_begin);
    if (!HasAsciiLetter(tail)) return {};

    Result resolved = ResolveExact(text + tail_begin, units - tail_begin);
    if (!resolved.text)
        resolved = ResolveCanonical(text + tail_begin, units - tail_begin);
    if (!resolved.text)
        resolved = ResolveNormalized(text + tail_begin, units - tail_begin);
    if (!resolved.text) return {};

    scratch->assign(text, tail_begin);
    scratch->append(resolved.text);
    return {scratch->c_str(), static_cast<uint32_t>(units),
            Mode::MixedLocalizedField};
}

std::wstring TextCoverageResolver::NormalizeWhitespace(const wchar_t* text,
                                                       size_t units) {
    std::wstring output;
    output.reserve(units);
    bool pending_space = false;
    for (size_t index = 0; index < units; ++index) {
        if (IsSpace(text[index])) {
            if (!output.empty()) pending_space = true;
            continue;
        }
        if (pending_space && !output.empty()) output.push_back(L' ');
        pending_space = false;
        output.push_back(text[index]);
    }
    return output;
}

std::wstring TextCoverageResolver::CanonicalizeLookupSource(
    const wchar_t* text, size_t units) {
    std::wstring value=NormalizeWhitespace(text,units);

    // First normalize speaker separator spacing. This keeps the key stable for
    // "Secretary : text", "Secretary: text" and full-width-colon variants.
    const size_t limit=std::min<size_t>(64u,value.size());
    const size_t ascii_colon=value.find(L':',1u);
    const size_t wide_colon=value.find(static_cast<wchar_t>(0xFF1A),1u);
    size_t colon=std::wstring::npos;
    if (ascii_colon!=std::wstring::npos) colon=ascii_colon;
    if (wide_colon!=std::wstring::npos &&
        (colon==std::wstring::npos || wide_colon<colon)) colon=wide_colon;
    if (colon!=std::wstring::npos && colon<limit) {
        bool speaker_like=false;
        for (size_t i=0;i<colon;++i) {
            const wchar_t c=value[i];
            if ((c>=L'A'&&c<=L'Z') || (c>=L'a'&&c<=L'z')) {
                speaker_like=true;
                break;
            }
        }
        if (speaker_like) {
            size_t speaker_end=colon;
            while (speaker_end>0u && IsSpace(value[speaker_end-1u])) --speaker_end;
            size_t body_begin=colon+1u;
            while (body_begin<value.size() && IsSpace(value[body_begin])) ++body_begin;
            if (speaker_end!=0u && body_begin<value.size()) {
                std::wstring normalized=value.substr(0,speaker_end);
                normalized.push_back(L':');
                normalized.push_back(L' ');
                normalized.append(value,body_begin,std::wstring::npos);
                value.swap(normalized);
            }
        }
    }

    // R9 source canonicalization is deliberately broader but still deterministic:
    // ASCII case and common English punctuation variants are folded. Build-time
    // collision elimination in BuildCanonicalAliases still rejects any canonical
    // key that would lead to different LOC replacements, so this cannot become a
    // first-candidate guess.
    std::wstring folded;
    folded.reserve(value.size()+8u);
    for (wchar_t c:value) {
        if (c>=L'A' && c<=L'Z') c=static_cast<wchar_t>(c+(L'a'-L'A'));
        const uint32_t u=static_cast<uint32_t>(c);
        if (u==0xFF1Au) { folded.push_back(L':'); continue; }
        if (u==0x2018u || u==0x2019u) { folded.push_back(L'\''); continue; }
        if (u==0x201Cu || u==0x201Du) { folded.push_back(L'"'); continue; }
        if (u==0x2013u || u==0x2014u) { folded.push_back(L'-'); continue; }
        if (u==0x2026u) { folded.append(L"..."); continue; }
        folded.push_back(c);
    }
    return NormalizeWhitespace(folded.c_str(),folded.size());
}

bool TextCoverageResolver::HasAsciiLetter(const std::wstring& text) {
    for (wchar_t c : text) {
        if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'))
            return true;
    }
    return false;
}

bool TextCoverageResolver::HasCjkSignal(const std::wstring& text) {
    for (wchar_t c : text) {
        const uint32_t u = static_cast<uint32_t>(c);
        if ((u >= 0x3400u && u <= 0x4DBFu) ||
            (u >= 0x4E00u && u <= 0x9FFFu) ||
            (u >= 0xF900u && u <= 0xFAFFu)) {
            return true;
        }
    }
    return false;
}

size_t TextCoverageResolver::CountAsciiLetters(const std::wstring& text) {
    size_t count = 0;
    for (wchar_t c : text) {
        if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z')) ++count;
    }
    return count;
}

size_t TextCoverageResolver::CountCjkSignals(const std::wstring& text) {
    size_t count = 0;
    for (wchar_t c : text) {
        const uint32_t u = static_cast<uint32_t>(c);
        if ((u >= 0x3400u && u <= 0x4DBFu) ||
            (u >= 0x4E00u && u <= 0x9FFFu) ||
            (u >= 0xF900u && u <= 0xFAFFu)) {
            ++count;
        }
    }
    return count;
}

bool TextCoverageResolver::IsMeaningfulReplacement(
    const std::wstring& source, const std::wstring& replacement) {
    if (source.empty() || replacement.empty() || source == replacement)
        return false;
    if (HasAsciiLetter(source) && !HasCjkSignal(replacement))
        return false;
    return true;
}

bool TextCoverageResolver::IsSafeSourceFallbackReplacement(
    const std::wstring& source, const std::wstring& replacement) {
    if (!IsMeaningfulReplacement(source, replacement)) return false;

    // 这里只约束“脱离原 key/hash 的 source fallback 索引”，不否定 LOC 本身。
    // 长英语若 replacement 仍保留绝大多数英语字母、却只混入极少 CJK，通常
    // 说明是运行时局部拼接/旧索引污染，不应再作为整句 source fallback。
    const size_t source_ascii = CountAsciiLetters(source);
    if (source_ascii < 24u) return true;
    const size_t replacement_ascii = CountAsciiLetters(replacement);
    const size_t replacement_cjk = CountCjkSignals(replacement);
    if (replacement_ascii >= 16u && replacement_ascii * 100u >= source_ascii * 70u &&
        replacement_cjk <= 4u) {
        return false;
    }
    return true;
}

bool TextCoverageResolver::IsSpace(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}

bool TextCoverageResolver::IsAsciiAlphaString(const std::wstring& text) {
    if (text.empty()) return false;
    for (wchar_t value : text) {
        if (!((value >= L'A' && value <= L'Z') ||
              (value >= L'a' && value <= L'z'))) {
            return false;
        }
    }
    return true;
}

bool TextCoverageResolver::IsNeutralSpeakerIdentifier(
    const std::wstring& text) {
    if (text.empty() || text.size() > 4u) return false;
    bool has_marker = false;
    for (wchar_t value : text) {
        if ((value >= L'A' && value <= L'Z') ||
            (value >= L'0' && value <= L'9')) {
            has_marker = true;
            continue;
        }
        if (value == L'.' || value == L'-' || value == L'_') continue;
        return false;
    }
    return has_marker;
}


bool TextCoverageResolver::IsAlreadyLocalizedBody(const std::wstring& text) {
    const size_t cjk = CountCjkSignals(text);
    if (cjk == 0u) return false;
    const size_t ascii = CountAsciiLetters(text);
    // 允许中文正文保留 MI6/P99/Smartphone 等技术标识，但拒绝“整段英语只
    // 混入一个中文字”的已知污染形态。
    return ascii <= 12u || ascii <= cjk * 2u + 8u;
}

bool TextCoverageResolver::IsPrintfConversion(wchar_t c) {
    switch (c) {
        case L'd': case L'i': case L'u': case L'o': case L'x': case L'X':
        case L'f': case L'F': case L'e': case L'E': case L'g': case L'G':
        case L'a': case L'A': case L'c': case L'C': case L's': case L'S':
        case L'p': case L'n':
            return true;
        default:
            return false;
    }
}

bool TextCoverageResolver::ParsePrintfTemplate(
    const std::wstring& text, std::vector<FormatPart>* parts,
    size_t* placeholder_count) {
    if (!parts || !placeholder_count) return false;
    parts->clear();
    *placeholder_count = 0;
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
        while (end < text.size() && !IsPrintfConversion(text[end])) {
            const wchar_t c = text[end];
            const bool modifier = (c >= L'0' && c <= L'9') || c == L'$' ||
                c == L'-' || c == L'+' || c == L' ' || c == L'#' ||
                c == L'.' || c == L'*' || c == L'h' || c == L'l' ||
                c == L'j' || c == L'z' || c == L't' || c == L'L' ||
                c == L'I';
            if (!modifier) return false;
            ++end;
        }
        if (end >= text.size() || !IsPrintfConversion(text[end])) return false;
        if (!literal.empty()) {
            parts->push_back({std::move(literal), false, 0});
            literal.clear();
        }
        parts->push_back({std::wstring(), true, text[end]});
        ++(*placeholder_count);
        index = end + 1u;
    }
    if (!literal.empty() || parts->empty())
        parts->push_back({std::move(literal), false, 0});
    return true;
}

bool TextCoverageResolver::ParseTemplateTokenAt(
    const wchar_t* text, size_t units, size_t begin, size_t* end) {
    if (!text || !end || begin >= units) return false;
    size_t cursor = begin;
    bool wrapped = false;
    if (cursor + 2u < units && text[cursor] == L'~' &&
        text[cursor + 1u] == L'~' && text[cursor + 2u] == L'{') {
        wrapped = true;
        cursor += 3u;
    } else if (text[cursor] == L'{') {
        ++cursor;
    } else {
        return false;
    }
    const size_t name_begin = cursor;
    while (cursor < units && cursor - name_begin <= 64u) {
        if (text[cursor] == L'}') {
            if (cursor == name_begin) return false;
            ++cursor;
            if (wrapped) {
                if (cursor + 1u >= units || text[cursor] != L'~' ||
                    text[cursor + 1u] != L'~') {
                    return false;
                }
                cursor += 2u;
            }
            *end = cursor;
            return true;
        }
        if (!IsTokenNameUnit(text[cursor])) return false;
        ++cursor;
    }
    return false;
}

bool TextCoverageResolver::ParseTokenTemplate(
    const std::wstring& text, std::vector<FormatPart>* parts,
    size_t* placeholder_count) {
    if (!parts || !placeholder_count) return false;
    parts->clear();
    *placeholder_count = 0;
    std::wstring literal;
    for (size_t index = 0; index < text.size();) {
        size_t token_end = 0;
        if (!ParseTemplateTokenAt(text.c_str(), text.size(), index,
                                  &token_end)) {
            literal.push_back(text[index++]);
            continue;
        }
        if (!literal.empty()) {
            parts->push_back({std::move(literal), false, 0});
            literal.clear();
        }
        parts->push_back({std::wstring(), true, L'T'});
        ++(*placeholder_count);
        index = token_end;
    }
    if (!literal.empty() || parts->empty())
        parts->push_back({std::move(literal), false, 0});
    return true;
}

bool TextCoverageResolver::MatchTemplate(
    const std::vector<FormatPart>& parts, const wchar_t* text, size_t units,
    std::vector<std::wstring>* captures) {
    if (!text || !captures) return false;
    captures->clear();
    size_t at = 0;
    for (size_t index = 0; index < parts.size(); ++index) {
        const FormatPart& part = parts[index];
        if (!part.placeholder) {
            if (part.literal.size() > units - at ||
                wmemcmp(text + at, part.literal.data(), part.literal.size()) != 0)
                return false;
            at += part.literal.size();
            continue;
        }
        const FormatPart* next_literal = nullptr;
        for (size_t next = index + 1u; next < parts.size(); ++next) {
            if (!parts[next].placeholder) {
                next_literal = &parts[next];
                break;
            }
        }
        size_t end = units;
        if (next_literal && !next_literal->literal.empty()) {
            const std::wstring haystack(text, units);
            end = haystack.find(next_literal->literal, at);
            if (end == std::wstring::npos) return false;
        }
        if (end < at) return false;
        captures->emplace_back(text + at, end - at);
        at = end;
    }
    return at == units;
}

bool TextCoverageResolver::ComposePrintf(
    const TemplateRule& rule, const std::vector<std::wstring>& captures,
    std::wstring* output) const {
    if (!output) return false;
    output->clear();
    size_t capture_index = 0;
    for (const FormatPart& part : rule.replacement_parts) {
        if (!part.placeholder) {
            output->append(part.literal);
            continue;
        }
        if (capture_index >= captures.size()) return false;
        const std::wstring& capture = captures[capture_index];
        wchar_t source_type = 0;
        size_t seen = 0;
        for (const FormatPart& source_part : rule.source_parts) {
            if (!source_part.placeholder) continue;
            if (seen++ == capture_index) {
                source_type = source_part.placeholder_type;
                break;
            }
        }
        if (source_type == L's' || source_type == L'S') {
            const auto exact = exact_.find(capture);
            if (exact != exact_.end()) {
                output->append(exact->second);
            } else {
                const std::wstring canonical_key =
                    CanonicalizeLookupSource(capture.c_str(), capture.size());
                const auto canonical = canonical_.find(canonical_key);
                if (canonical != canonical_.end()) {
                    output->append(canonical->second);
                } else {
                    const std::wstring key =
                        NormalizeWhitespace(capture.c_str(), capture.size());
                    const auto normalized = normalized_.find(key);
                    if (normalized != normalized_.end())
                        output->append(normalized->second);
                    else
                        output->append(capture);
                }
            }
        } else {
            output->append(capture);
        }
        ++capture_index;
    }
    return capture_index == captures.size();
}

bool TextCoverageResolver::ComposeTokenTemplate(
    const std::vector<FormatPart>& replacement_parts,
    const std::vector<std::wstring>& captures, std::wstring* output) {
    if (!output) return false;
    output->clear();
    size_t capture_index = 0;
    for (const FormatPart& part : replacement_parts) {
        if (!part.placeholder) {
            output->append(part.literal);
        } else {
            if (capture_index >= captures.size()) return false;
            output->append(captures[capture_index++]);
        }
    }
    return capture_index == captures.size();
}

bool TextCoverageResolver::ParseWrappedControlTokenAt(
    const wchar_t* text, size_t units, size_t begin, size_t* end,
    size_t* name_begin, size_t* name_end) {
    if (!text || !end || begin + 7u > units || text[begin] != L'~' ||
        text[begin + 1u] != L'~' || text[begin + 2u] != L'{') {
        return false;
    }
    size_t cursor = begin + 3u;
    const size_t name_start = cursor;
    while (cursor < units && cursor - name_start <= 64u) {
        if (text[cursor] == L'}') {
            if (cursor == name_start || cursor + 2u >= units ||
                text[cursor + 1u] != L'~' || text[cursor + 2u] != L'~') {
                return false;
            }
            if (name_begin) *name_begin = name_start;
            if (name_end) *name_end = cursor;
            *end = cursor + 3u;
            return true;
        }
        if (!IsTokenNameUnit(text[cursor])) return false;
        ++cursor;
    }
    return false;
}

bool TextCoverageResolver::FindWrappedControlTokenEndingAt(
    const wchar_t* text, size_t minimum_begin, size_t end,
    size_t* token_begin) {
    if (!text || !token_begin || end < minimum_begin + 7u ||
        text[end - 3u] != L'}' || text[end - 2u] != L'~' ||
        text[end - 1u] != L'~') {
        return false;
    }
    const size_t earliest = end > 70u ? std::max(minimum_begin, end - 70u)
                                      : minimum_begin;
    size_t cursor = end - 3u;
    while (cursor > earliest) {
        --cursor;
        if (text[cursor] != L'{') continue;
        if (cursor < 2u || cursor - 2u < minimum_begin ||
            text[cursor - 2u] != L'~' || text[cursor - 1u] != L'~' ||
            cursor + 1u == end - 3u) {
            return false;
        }
        for (size_t unit = cursor + 1u; unit < end - 3u; ++unit) {
            if (!IsTokenNameUnit(text[unit])) return false;
        }
        *token_begin = cursor - 2u;
        return true;
    }
    return false;
}

bool TextCoverageResolver::SplitWrappedControlTokenBody(
    const wchar_t* text, size_t units, size_t* body_begin, size_t* body_end) {
    if (!text || !body_begin || !body_end || units < 7u) return false;
    size_t cursor = 0;
    while (cursor < units && (text[cursor] == L' ' || text[cursor] == L'\t'))
        ++cursor;
    size_t token_end = 0;
    size_t leading = 0;
    while (ParseWrappedControlTokenAt(text, units, cursor, &token_end)) {
        ++leading;
        cursor = token_end;
        while (cursor < units &&
               (text[cursor] == L' ' || text[cursor] == L'\t')) {
            ++cursor;
        }
    }
    *body_begin = leading ? cursor : 0u;

    cursor = units;
    while (cursor > *body_begin &&
           (text[cursor - 1u] == L' ' || text[cursor - 1u] == L'\t')) {
        --cursor;
    }
    size_t trailing = 0;
    size_t token_begin = 0;
    while (FindWrappedControlTokenEndingAt(text, *body_begin, cursor,
                                           &token_begin)) {
        ++trailing;
        cursor = token_begin;
        while (cursor > *body_begin &&
               (text[cursor - 1u] == L' ' || text[cursor - 1u] == L'\t')) {
            --cursor;
        }
    }
    *body_end = trailing ? cursor : units;
    return (leading != 0u || trailing != 0u) && *body_begin < *body_end;
}

bool TextCoverageResolver::LooksLikeControlIsland(
    const std::wstring& capture) {
    size_t begin = 0;
    size_t end = capture.size();
    while (begin < end && IsSpace(capture[begin])) ++begin;
    while (end > begin && IsSpace(capture[end - 1u])) --end;
    if (begin == end || end - begin > 64u) return false;
    const std::wstring trimmed = capture.substr(begin, end - begin);
    // 明确的模板 token 始终可接受。除此之外，非 ASCII 字符本身不能
    // 作为“这是控制键”的证据：否则普通中文碎片（例如某个已被提前替换的
    // 连接词）会被误当作动态岛，再次造成“整句英文中混入一个中文字”。
    // PUA 范围用于兼容游戏/伴随插件可能生成的私用区按键图标。
    bool has_explicit_token = false;
    bool has_pua_marker = false;
    for (size_t index = 0; index < trimmed.size(); ++index) {
        size_t token_end = 0;
        if (ParseTemplateTokenAt(trimmed.c_str(), trimmed.size(), index,
                                 &token_end)) {
            has_explicit_token = true;
            break;
        }
        const uint32_t unit = static_cast<uint32_t>(trimmed[index]);
        if (unit >= 0xE000u && unit <= 0xF8FFu) has_pua_marker = true;
    }
    if (has_explicit_token || has_pua_marker) return true;
    if (trimmed.size() > 24u) return false;

    bool ascii_key_marker = false;
    for (wchar_t value : trimmed) {
        if ((value >= L'A' && value <= L'Z') ||
            (value >= L'0' && value <= L'9')) {
            ascii_key_marker = true;
        }
        // 非 ASCII 只允许作为一个已经含有 ASCII 键名的短组合中的连接/
        // 装饰单元存在；它绝不能独立让本捕获通过。这样可兼容“L <连接词> R”
        // 一类运行时组合，同时拒绝孤立中文碎片。
        if (static_cast<uint32_t>(value) >= 0x80u) continue;
        const bool allowed =
            (value >= L'A' && value <= L'Z') ||
            (value >= L'a' && value <= L'z') ||
            (value >= L'0' && value <= L'9') || value == L' ' ||
            value == L'+' || value == L'/' || value == L'|' ||
            value == L'&' || value == L'-' || value == L'(' ||
            value == L')';
        if (!allowed) return false;
    }
    return ascii_key_marker;
}

bool TextCoverageResolver::SplitSpeakerBody(const std::wstring& text,
                                            size_t* separator,
                                            size_t* body_begin) {
    if (!separator || !body_begin) return false;
    const size_t limit = std::min<size_t>(64u, text.size());
    const size_t ascii_colon = text.find(L':', 1u);
    const size_t wide_colon = text.find(static_cast<wchar_t>(0xFF1A), 1u);
    size_t colon = std::wstring::npos;
    if (ascii_colon != std::wstring::npos) colon = ascii_colon;
    if (wide_colon != std::wstring::npos &&
        (colon == std::wstring::npos || wide_colon < colon)) {
        colon = wide_colon;
    }
    if (colon == std::wstring::npos || colon >= limit) return false;
    size_t speaker_end = colon;
    while (speaker_end > 0u && IsSpace(text[speaker_end - 1u]))
        --speaker_end;
    size_t body = colon + 1u;
    while (body < text.size() && IsSpace(text[body])) ++body;
    if (speaker_end == 0u || body >= text.size()) return false;
    *separator = speaker_end;
    *body_begin = body;
    return true;
}

bool TextCoverageResolver::FindStructuredTail(const std::wstring& text,
                                              size_t* tail_begin) {
    if (!tail_begin || text.size() < 3u) return false;
    // 优先最后一个冒号；任务/提示在前半已经本地化后常形成“中文前缀：English”。
    const size_t ascii_colon = text.rfind(L':');
    const size_t wide_colon = text.rfind(static_cast<wchar_t>(0xFF1A));
    size_t delimiter = std::wstring::npos;
    if (ascii_colon != std::wstring::npos) delimiter = ascii_colon;
    if (wide_colon != std::wstring::npos &&
        (delimiter == std::wstring::npos || wide_colon > delimiter)) {
        delimiter = wide_colon;
    }
    size_t begin = std::wstring::npos;
    if (delimiter != std::wstring::npos && delimiter + 1u < text.size()) {
        begin = delimiter + 1u;
    } else {
        const size_t dash = text.rfind(L" - ");
        if (dash != std::wstring::npos) begin = dash + 3u;
    }
    if (begin == std::wstring::npos) return false;
    while (begin < text.size() && IsSpace(text[begin])) ++begin;
    if (begin >= text.size()) return false;
    *tail_begin = begin;
    return true;
}

void TextCoverageResolver::ObserveWrappedTokens(const wchar_t* text,
                                                size_t units) const {
    if (!log_action_tokens_) return;
    for (size_t index = 0; index < units; ++index) {
        size_t token_end = 0;
        size_t name_begin = 0;
        size_t name_end = 0;
        if (!ParseWrappedControlTokenAt(text, units, index, &token_end,
                                        &name_begin, &name_end)) {
            continue;
        }
        const std::wstring name(text + name_begin, name_end - name_begin);
        bool inserted = false;
        size_t total = 0;
        {
            std::lock_guard<std::mutex> lock(observed_token_mutex_);
            inserted = observed_action_tokens_.insert(name).second;
            total = observed_action_tokens_.size();
        }
        if (inserted) {
            BSCN_LOG_INFO("ACTION_TOKEN_SEEN syntax=~~{NAME}~~ name=%s unique=%u",
                          WideToUtf8(name).c_str(),
                          static_cast<unsigned>(total));
        }
        index = token_end - 1u;
    }
}

const char* TextCoverageResolver::ModeName(Mode mode) {
    switch (mode) {
        case Mode::Exact: return "exact";
        case Mode::PrintfTemplate: return "printf_template";
        case Mode::PrintfAgreement: return "printf_agreement";
        case Mode::CompoundSegments: return "compound_segments";
        case Mode::ActionPromptBody: return "action_prompt_body_ap1";
        case Mode::TutorialDynamicTokens: return "tutorial_dynamic_tokens";
        case Mode::SpeakerAndBody: return "speaker_and_body";
        case Mode::MixedLocalizedField: return "mixed_localized_field";
        case Mode::WhitespaceNormalized: return "whitespace_normalized";
        case Mode::CanonicalSource: return "canonical_source";
        default: return "none";
    }
}

}  // namespace bscn
