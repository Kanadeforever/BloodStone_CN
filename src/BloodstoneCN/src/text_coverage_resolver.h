#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bscn {

class ExternalLocOverrides;

// 最终 SubmitText / UIDialogue 的数据驱动覆盖层。
//
// 这里不保存任何人工新增译文：所有 source/replacement 都在初始化时从
// 外置 BSCNLI3 + 当前 86 个 loose LOC 取得。LOC 是最终中文正文的唯一权威源。
class TextCoverageResolver {
public:
    enum class Mode : uint8_t {
        None = 0,
        Exact,
        PrintfTemplate,
        PrintfAgreement,
        CompoundSegments,
        ActionPromptBody,
        TutorialDynamicTokens,
        SpeakerAndBody,
        MixedLocalizedField,
        WhitespaceNormalized,
        CanonicalSource,
    };

    struct Result {
        const wchar_t* text = nullptr;
        uint32_t source_units = 0;
        Mode mode = Mode::None;
    };

    struct Stats {
        size_t records = 0;
        size_t unique_sources = 0;
        size_t ambiguous_sources = 0;
        size_t authoritative_loc_sources = 0;
        size_t skipped_ambiguous_loc_sources = 0;
        size_t rejected_partial_english_sources = 0;
        size_t canonical_sources = 0;
        size_t ambiguous_canonical_sources = 0;
        size_t safe_all_record_printf_templates = 0;
        size_t all_record_printf_candidates = 0;
        size_t prefix_alias_sources = 0;
        size_t base_label_alias_sources = 0;
        size_t unique_normalized_sources = 0;
        size_t ambiguous_normalized_sources = 0;
        size_t printf_templates = 0;
        size_t tutorial_templates = 0;
        size_t subtitle_bodies = 0;
        size_t ambiguous_subtitle_bodies = 0;
        size_t speaker_aliases = 0;
        size_t ambiguous_speaker_aliases = 0;
    };

    bool Initialize(const std::wstring& bli_path, const ExternalLocOverrides* loc,
                    bool log_action_tokens = true);
    void Shutdown();

    bool ready() const { return ready_; }
    const Stats& stats() const { return stats_; }
    Result Resolve(const wchar_t* submitted, std::wstring* scratch) const;
    static const char* ModeName(Mode mode);

private:
    struct RawRecord {
        uint32_t key_hash = 0;
        uint32_t flags = 0;
        std::wstring source;
        // BLI3 replacement 只作为同 hash 多值 LOC 的历史语境指纹。
        std::wstring indexed_replacement;
        // 当前 86 个 loose LOC 中按该 hash/context 取回的权威 replacement。
        std::wstring replacement;
    };

    struct FormatPart {
        std::wstring literal;
        bool placeholder = false;
        wchar_t placeholder_type = 0;
    };

    struct TemplateRule {
        std::wstring source;
        std::vector<FormatPart> source_parts;
        std::vector<FormatPart> replacement_parts;
        size_t placeholder_count = 0;
        size_t literal_units = 0;
    };

    static std::wstring NormalizeWhitespace(const wchar_t* text,
                                            size_t units);
    static std::wstring CanonicalizeLookupSource(const wchar_t* text,
                                                 size_t units);
    static bool HasAsciiLetter(const std::wstring& text);
    static bool HasCjkSignal(const std::wstring& text);
    static size_t CountAsciiLetters(const std::wstring& text);
    static size_t CountCjkSignals(const std::wstring& text);
    static bool IsMeaningfulReplacement(const std::wstring& source,
                                        const std::wstring& replacement);
    static bool IsSafeSourceFallbackReplacement(
        const std::wstring& source, const std::wstring& replacement);
    static bool IsSpace(wchar_t c);
    static bool IsPrintfConversion(wchar_t c);
    static bool IsAsciiAlphaString(const std::wstring& text);
    static bool IsNeutralSpeakerIdentifier(const std::wstring& text);
    static bool IsAlreadyLocalizedBody(const std::wstring& text);

    static bool ParsePrintfTemplate(const std::wstring& text,
                                    std::vector<FormatPart>* parts,
                                    size_t* placeholder_count);
    static bool ParseTemplateTokenAt(const wchar_t* text, size_t units,
                                     size_t begin, size_t* end);
    static bool ParseTokenTemplate(const std::wstring& text,
                                   std::vector<FormatPart>* parts,
                                   size_t* placeholder_count);
    static bool MatchTemplate(const std::vector<FormatPart>& parts,
                              const wchar_t* text, size_t units,
                              std::vector<std::wstring>* captures);
    bool ComposePrintf(const TemplateRule& rule,
                       const std::vector<std::wstring>& captures,
                       std::wstring* output) const;
    static bool ComposeTokenTemplate(
        const std::vector<FormatPart>& replacement_parts,
        const std::vector<std::wstring>& captures,
        std::wstring* output);

    static bool ParseWrappedControlTokenAt(const wchar_t* text, size_t units,
                                           size_t begin, size_t* end,
                                           size_t* name_begin = nullptr,
                                           size_t* name_end = nullptr);
    static bool FindWrappedControlTokenEndingAt(const wchar_t* text,
                                                size_t minimum_begin,
                                                size_t end,
                                                size_t* token_begin);
    static bool SplitWrappedControlTokenBody(const wchar_t* text, size_t units,
                                             size_t* body_begin,
                                             size_t* body_end);
    static bool LooksLikeControlIsland(const std::wstring& capture);
    static bool SplitSpeakerBody(const std::wstring& text,
                                 size_t* separator,
                                 size_t* body_begin);
    static bool FindStructuredTail(const std::wstring& text,
                                   size_t* tail_begin);

    void ObserveWrappedTokens(const wchar_t* text, size_t units) const;
    void BuildShortPrefixAliases(const std::vector<RawRecord>& records);
    void BuildBaseLabelAliases(const std::vector<RawRecord>& records);
    void BuildNormalizedAliases(const std::vector<RawRecord>& records);
    void BuildCanonicalAliases(const std::vector<RawRecord>& records);
    void BuildSubtitleBodyIndex(const std::vector<RawRecord>& records);
    void BuildSafeAllRecordPrintfRules(const std::vector<RawRecord>& records);
    void BuildAllRecordPrintfCandidates(const std::vector<RawRecord>& records);

    Result ResolveExact(const wchar_t* text, size_t units) const;
    Result ResolveNormalized(const wchar_t* text, size_t units) const;
    Result ResolveCanonical(const wchar_t* text, size_t units) const;
    Result ResolvePrintf(const wchar_t* text, size_t units,
                         std::wstring* scratch) const;
    Result ResolvePrintfAgreement(const wchar_t* text, size_t units,
                                  std::wstring* scratch) const;
    Result ResolveCompound(const wchar_t* text, size_t units,
                           std::wstring* scratch) const;
    bool ComposeCompoundSegments(const wchar_t* text, size_t units,
                                 std::wstring* output) const;
    Result ResolveActionPrompt(const wchar_t* text, size_t units,
                               std::wstring* scratch) const;
    Result ResolveTutorial(const wchar_t* text, size_t units,
                           std::wstring* scratch) const;
    Result ResolveSpeaker(const wchar_t* text, size_t units,
                          std::wstring* scratch) const;
    Result ResolveMixedLocalizedField(const wchar_t* text, size_t units,
                                      std::wstring* scratch) const;

    bool ready_ = false;
    bool log_action_tokens_ = true;
    Stats stats_{};
    std::unordered_map<std::wstring, std::wstring> exact_;
    std::unordered_map<std::wstring, std::wstring> normalized_;
    std::unordered_map<std::wstring, std::wstring> canonical_;
    std::unordered_map<std::wstring, std::wstring> subtitle_bodies_;
    std::unordered_map<std::wstring, std::wstring> speaker_names_;
    std::vector<TemplateRule> printf_rules_;
    std::vector<TemplateRule> all_printf_rules_;
    std::vector<TemplateRule> tutorial_rules_;

    mutable std::mutex observed_token_mutex_;
    mutable std::unordered_set<std::wstring> observed_action_tokens_;
};

}  // namespace bscn
