#pragma once
#include "common.h"
#include "config.h"

namespace bscn {

// 外置本地化层。
//
// LOC 是运行时中文内容的权威来源；BSCNLI3 只负责保存原英语、key/hash、
// 冲突语境与 DEFER_TO_SUBMIT_TEXT 等辅助信息。二者均从磁盘读取，ASI 中
// 不包含任何翻译正文。
class ExternalLocOverrides {
public:
    struct IndexedOverride {
        uint32_t key_hash = 0;
        std::wstring source;
        std::wstring replacement;
        uint32_t flags = 0;
    };
    static constexpr uint32_t kIndexFlagDeferToSubmitText = 0x1u;

    bool Initialize(const std::wstring& module_directory, const Config& config);
    void Shutdown();

    // R9 字体就绪门。启用内置中文字体时，D3D atlas 尚未建立/Reset 中
    // 暂停返回中文，避免原字体把中文渲染成方框。
    void SetRenderingReady(bool ready);
    bool rendering_ready() const { return rendering_ready_.load(); }

    // GetText 路径：BLI3 精确语境优先；仅在该 hash 不存在 BLI3 记录时，
    // 才按 LOC hash 回退，避免跨关卡复用 hash 时猜错译文。
    const wchar_t* Resolve(uint32_t primary_hash, uint32_t secondary_hash,
                           const wchar_t* original = nullptr);

    // SubmitText 路径：先处理 M2F1 的同线程精确 defer，再尝试最近同线程、
    // 由 GetText 原始 key/hash 确认过的语境；最后才尝试 10 秒窗口内“唯一
    // 一致”的跨线程 defer。其余通用覆盖交给 TextCoverage。
    const wchar_t* ResolveSubmittedText(
        const wchar_t* submitted,
        uint32_t* authoritative_source_units = nullptr);

    size_t override_count() const { return loc_overrides_.size(); }
    size_t indexed_record_count() const { return records_.size(); }
    size_t loc_file_count() const { return loc_file_count_; }
    size_t valid_loc_file_count() const { return valid_loc_file_count_; }
    const std::wstring& index_path() const { return index_path_; }
    const std::wstring& root_path() const { return root_; }

    // TextCoverage 解析 BLI3 source 时，用此接口取得当前 loose LOC 对应的
    // 权威 replacement。BLI3 的 replacement 只参与多候选语境指纹匹配，
    // 绝不作为最终译文回退；当前 LOC 无值或多候选无法证明时返回空串。
    std::wstring CopyAuthoritativeReplacement(
        uint32_t key_hash, const std::wstring& indexed_replacement) const;

private:
    bool LoadIndex(const std::wstring& path);
    void CollectLocFiles(const std::wstring& directory, unsigned depth,
                         std::vector<std::wstring>* files) const;
    bool LoadLocFile(const std::wstring& path);

    // R10: native GetText source defines the identity family first. Runtime
    // hashes may only disambiguate records inside that source family.
    const IndexedOverride* SelectSourceFirstCandidate(
        uint32_t primary_hash, uint32_t secondary_hash,
        const wchar_t* original);
    const IndexedOverride* SelectHashFallbackCandidate(
        uint32_t hash, const wchar_t* original);
    const wchar_t* ResolveCandidate(const IndexedOverride& candidate,
                                    const wchar_t* original,
                                    uint32_t primary_hash,
                                    uint32_t secondary_hash);
    static bool ShouldAutoDeferToSubmitText(const std::wstring& source);
    const std::wstring* FindAuthoritativeReplacement(
        const IndexedOverride& candidate) const;
    void LogAuthoritativeLocProblem(uint32_t key_hash, size_t candidate_count,
                                    const char* reason) const;

    struct DeferredContext {
        const ExternalLocOverrides* owner = nullptr;
        DWORD tick = 0;
        DWORD thread_id = 0;
        uint64_t event_id = 0;
        uint32_t key_hash = 0;
        bool explicit_defer = false;
        std::wstring source;
        std::wstring replacement;
        std::wstring output;

        void Reset() {
            owner = nullptr;
            tick = 0;
            thread_id = 0;
            event_id = 0;
            key_hash = 0;
            explicit_defer = false;
            source.clear();
            replacement.clear();
            output.clear();
        }
    };
    static DeferredContext& ThreadContext();

    struct RecentPreciseMapping {
        const IndexedOverride* mapping = nullptr;
        DWORD tick = 0;
        uint64_t sequence = 0;
    };
    struct PreciseThreadContext {
        const ExternalLocOverrides* owner = nullptr;
        uint64_t next_sequence = 0;
        std::array<RecentPreciseMapping, 64> recent{};
        std::wstring output_a;
        std::wstring output_b;

        void Reset(const ExternalLocOverrides* new_owner = nullptr) {
            owner = new_owner;
            next_sequence = 0;
            for (RecentPreciseMapping& entry : recent) entry = {};
            output_a.clear();
            output_b.clear();
        }
    };
    static PreciseThreadContext& PreciseContext();
    void RememberPreciseMapping(const IndexedOverride& candidate);
    const wchar_t* ApplyRecentPreciseMappings(const std::wstring& submitted);

    // 电影/过场字幕的 GetText 与最终 SubmitText 可能发生在线程切换前后。
    // 同线程 DeferredContext 继续作为最高优先级；下面这个小型全局环只保存
    // 已由原始 key/hash 精确确认且明确标记 DEFER_TO_SUBMIT_TEXT 的映射。
    // 消费时要求时间窗内唯一一致，绝不按“第一候选”猜测。
    struct GlobalDeferredMapping {
        DWORD tick = 0;
        DWORD thread_id = 0;
        uint64_t sequence = 0;
        uint64_t event_id = 0;
        uint32_t key_hash = 0;
        bool explicit_defer = false;
        std::wstring source;
        std::wstring replacement;
    };
    struct CrossThreadOutputContext {
        const ExternalLocOverrides* owner = nullptr;
        std::wstring output;

        void Reset(const ExternalLocOverrides* new_owner = nullptr) {
            owner = new_owner;
            output.clear();
        }
    };
    static CrossThreadOutputContext& CrossThreadContext();
    void RememberGlobalDeferred(uint64_t event_id, uint32_t key_hash,
                                bool explicit_defer,
                                const std::wstring& source,
                                const std::wstring& replacement);
    const wchar_t* ApplyGlobalDeferred(
        const std::wstring& submitted,
        uint32_t* authoritative_source_units);

    static bool ParseTemplateTokenAt(const std::wstring& text, size_t begin,
                                     size_t* token_end);
    static bool LooksLikeControlIsland(const std::wstring& capture);
    static bool MatchDynamicTemplate(
        const std::wstring& input, const std::wstring& source,
        std::vector<std::pair<size_t, size_t>>* captures);
    static bool BuildDynamicReplacement(
        const std::wstring& input, const std::wstring& replacement,
        const std::vector<std::pair<size_t, size_t>>& captures,
        std::wstring* output);

    // GetText 已按原始 key/hash 精确选中格式模板后，游戏可能先把 %s/%d 等
    // 参数格式化，再进入 SubmitText。这里仅在这个“精确语境模板”上恢复
    // 运行时参数，不把歧义模板提升为全局 source fallback。
    struct PrintfTemplateParts {
        std::vector<std::wstring> literals;
        std::vector<wchar_t> conversions;
    };
    static bool ParsePrintfTemplate(const std::wstring& text,
                                    PrintfTemplateParts* parts);
    static bool MatchPrintfTemplate(
        const std::wstring& input, const std::wstring& source,
        std::vector<std::wstring>* captures);
    static bool BuildPrintfReplacement(
        const std::wstring& replacement,
        const std::vector<std::wstring>& captures, std::wstring* output);

    bool enabled_ = false;
    bool hud_bypass_dynamic_scalars_ = true;
    bool hud_defer_technical_labels_ = true;
    std::atomic<bool> rendering_ready_{true};
    std::wstring root_;
    std::wstring index_path_;
    wchar_t language_[2] = {L'e', L'n'};
    size_t loc_file_count_ = 0;
    size_t valid_loc_file_count_ = 0;

    std::unordered_map<uint32_t, std::wstring> loc_overrides_;
    // 当前 86 个 LOC 中同一 hash 的全部不同 replacement。loc_overrides_
    // 继续保留历史“最后一个值”用于无 BLI hash fallback；source-context
    // 路径必须看完整候选集合，不能再把多语境 hash 压成一个字符串。
    std::unordered_map<uint32_t, std::vector<std::wstring>> loc_candidates_;
    std::vector<IndexedOverride> records_;
    std::unordered_map<uint32_t, std::vector<size_t>> by_hash_;
    std::unordered_map<std::wstring, std::vector<size_t>> by_source_;

    std::array<GlobalDeferredMapping, 64> global_deferred_{};
    std::mutex global_deferred_mutex_;
    uint64_t global_deferred_sequence_ = 0;
    std::atomic<uint64_t> deferred_event_sequence_{0};

    std::atomic<bool> ambiguous_miss_logged_{false};
    std::mutex source_mismatch_log_mutex_;
    std::unordered_set<uint32_t> logged_source_mismatch_hashes_;
    std::atomic<bool> precise_submit_logged_{false};
    std::atomic<bool> precise_printf_logged_{false};
    std::atomic<bool> deferred_same_thread_logged_{false};
    std::atomic<bool> deferred_cross_thread_logged_{false};
    std::atomic<bool> auto_defer_logged_{false};
    std::mutex hud_identity_log_mutex_;
    std::unordered_set<uint32_t> logged_hud_identity_defer_keys_;
    std::atomic<bool> dynamic_scalar_gettext_logged_{false};
    std::mutex source_first_log_mutex_;
    std::unordered_set<uint64_t> logged_source_first_disagreements_;
    std::atomic<bool> font_gate_wait_logged_{false};
    mutable std::mutex authoritative_log_mutex_;
    mutable std::unordered_set<uint32_t> logged_authoritative_problem_hashes_;
};

}  // namespace bscn
