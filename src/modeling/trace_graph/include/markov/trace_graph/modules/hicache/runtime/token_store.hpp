/**
 * @file
 * @brief HiCache request-local token snapshot 目录和 path resolution。
 */
#pragma once

#include "markov/trace_graph/modules/hicache/fact.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

/**
 * @brief request token path 的完整性等级。
 *
 * 显式记录完整性可以把“输入缺 token”与“请求不足一个 page”区分开，避免模型静默
 * 少生成 target pages。
 */
enum class HiCacheTokenCompleteness { Unknown, Partial, PageAligned, Full };

/**
 * @brief token path snapshot 的语义阶段。
 *
 * 同一个 request_id 在 SGLang 中是一条增长的 token timeline；stage 用来明确
 * 当前 path 是 lookup key、cache extend path、lifecycle committed path，还是
 * prefetch candidate，避免跨阶段互相覆盖。
 */
enum class HiCacheTokenSnapshotStage {
    Unknown,
    CacheLookup,
    CacheExtend,
    LifecycleUnfinished,
    LifecycleFinished,
    PrefetchCandidate,
};

/**
 * @brief role-specific resolver 的解析结果状态。
 */
enum class HiCacheTokenResolutionStatus {
    Direct,
    Missing,
    WrongStageRejected,
    SourceClassRejected,
};

/** @brief 返回 token resolution status 的稳定诊断名。 */
[[nodiscard]] std::string hicache_token_resolution_status_name(HiCacheTokenResolutionStatus status);

/**
 * @brief 单个 fact 显式携带的 token path snapshot。
 *
 * snapshot 是不可变事实记录；directory 只保存它，不判断 state policy，也不推断
 * insert/prefetch/backup 是否应该发生。
 */
struct HiCacheTokenPathSnapshot {
    size_t source_event_index = 0;
    uint64_t seq_no = 0;
    uint64_t ts = 0;
    HiCacheTokenSnapshotStage stage = HiCacheTokenSnapshotStage::Unknown;
    uint64_t page_aligned_token_count = 0;
    HiCacheTokenCompleteness completeness = HiCacheTokenCompleteness::Unknown;
};

/**
 * @brief role-specific token path 解析结果。
 */
struct HiCacheTokenResolution {
    HiCacheTokenResolutionStatus status = HiCacheTokenResolutionStatus::Missing;
    HiCacheTokenPath tokens;
    uint64_t token_count = 0;
    uint64_t page_aligned_token_count = 0;

    /** @brief 当前解析结果是否可供 state model 消费。 */
    [[nodiscard]] bool ok() const { return status == HiCacheTokenResolutionStatus::Direct; }
};

/**
 * @brief batch-level `cache_extend_input` 的 token path 解析结果。
 */
struct HiCacheBatchTokenResolution {
    HiCacheTokenResolutionStatus status = HiCacheTokenResolutionStatus::Missing;
    std::vector<HiCacheTokenResolution> entries;

    /** @brief 当前 batch 是否每个 entry 都可直接消费。 */
    [[nodiscard]] bool ok() const { return status == HiCacheTokenResolutionStatus::Direct; }
};

/**
 * @brief request 维度的 token path snapshot directory。
 *
 * directory 显式保存 request timeline，不维护“当前最好 path”，也不允许 prefetch
 * candidate 覆盖 cache extend/lifecycle committed path。
 */
class HiCacheTokenDirectory {
public:
    /** @brief 返回 scope/request 复合 key；缺少 request_id 时返回空字符串。 */
    [[nodiscard]] std::string scoped_request_key(const HiCacheFact & fact) const;

    /** @brief 记录当前 fact 显式携带的 token path snapshot；缺 path 时不产生记录。 */
    void observe_fact_path(const HiCacheFact & fact, uint64_t page_size);

    /** @brief 解析 cache lookup path，必须来自当前 fact。 */
    [[nodiscard]] HiCacheTokenResolution resolve_cache_lookup_path(const HiCacheFact & fact, uint64_t page_size) const;

    /** @brief 解析 batch-level cache extend paths，必须来自当前 fact。 */
    [[nodiscard]] HiCacheBatchTokenResolution resolve_cache_extend_paths(const HiCacheFact & fact, uint64_t page_size) const;

    /** @brief 解析 finished/unfinished lifecycle committed path，必须来自当前 fact。 */
    [[nodiscard]] HiCacheTokenResolution resolve_cache_lifecycle_commit_path(const HiCacheFact & fact, uint64_t page_size) const;

    /** @brief 解析 prefetch candidate path；该 path 不会更新 request committed timeline。 */
    [[nodiscard]] HiCacheTokenResolution resolve_prefetch_candidate_path(const HiCacheFact & fact, uint64_t page_size) const;

    /** @brief 查询当前 fact 之前最近一次 committed cache snapshot。 */
    [[nodiscard]] const HiCacheTokenPathSnapshot * previous_committed_snapshot(const HiCacheFact & fact) const;

private:
    std::vector<HiCacheTokenPathSnapshot> snapshots_;
    std::unordered_map<std::string, std::vector<size_t>> snapshots_by_request_;

    /** @brief 记录一个已经通过合同检查的 scalar path snapshot。 */
    void append_snapshot(const HiCacheFact & fact, HiCacheTokenSnapshotStage stage, uint64_t page_size);
};

} // namespace markov::trace_graph::modules::hicache::runtime
