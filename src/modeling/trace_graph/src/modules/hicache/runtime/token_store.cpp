/**
 * @file
 * @brief HiCache request token path snapshot directory 实现。
 */
#include "markov/trace_graph/modules/hicache/runtime/token_store.hpp"

#include <utility>

namespace markov::trace_graph::modules::hicache::runtime {

namespace token_store_detail {

std::string normalized_scope(const HiCacheFact & fact) { return fact.cache_scope.empty() ? std::string("-1") : fact.cache_scope; }

uint8_t rank(HiCacheTokenCompleteness completeness) {
    switch (completeness) {
    case HiCacheTokenCompleteness::Unknown:
        return 0;
    case HiCacheTokenCompleteness::Partial:
        return 1;
    case HiCacheTokenCompleteness::PageAligned:
        return 2;
    case HiCacheTokenCompleteness::Full:
        return 3;
    }
    return 0;
}

HiCacheTokenCompleteness completeness_for_snapshot(const HiCacheFact & fact, uint64_t page_size) {
    if (!hicache_fact_has_resolved_full_path(fact)) return HiCacheTokenCompleteness::Unknown;
    if (fact.full_path_span.token_count == fact.full_path_tokens.size()) return HiCacheTokenCompleteness::Full;
    if (page_size > 0 && fact.full_path_tokens.size() / page_size * page_size == fact.full_path_tokens.size()) return HiCacheTokenCompleteness::PageAligned;
    return HiCacheTokenCompleteness::Partial;
}

uint64_t aligned_token_count(uint64_t token_count, uint64_t page_size) {
    if (page_size == 0) return 0;
    return token_count / page_size * page_size;
}

std::string lifecycle_stage_name(const std::string & lifecycle_kind) {
    if (lifecycle_kind == "finished") return "lifecycle_finished";
    if (lifecycle_kind == "unfinished") return "lifecycle_unfinished";
    return "lifecycle_unknown";
}

HiCacheTokenSnapshotStage stage_for_fact(const HiCacheFact & fact) {
    if (fact.role == "request_bound_match_anchor") return HiCacheTokenSnapshotStage::Match;
    if (fact.role == "request_admission") return HiCacheTokenSnapshotStage::Admission;
    if (fact.role == "prefetch_decision") return HiCacheTokenSnapshotStage::PrefetchCandidate;
    if (fact.role == "storage_backend_readable") return HiCacheTokenSnapshotStage::StorageReadable;
    if (fact.role == "request_lifecycle_anchor") {
        if (fact.lifecycle_kind == "finished") return HiCacheTokenSnapshotStage::LifecycleFinished;
        if (fact.lifecycle_kind == "unfinished") return HiCacheTokenSnapshotStage::LifecycleUnfinished;
    }
    return HiCacheTokenSnapshotStage::Unknown;
}

/**
 * @brief 判断 fact 是否允许写入 request token directory。
 *
 * directory 是 state model 的输入身份索引，不是诊断缓存。它只接受 completed
 * state-model path fact；source_actual/oracle 中的 path 即便更完整，也不能作为
 * target page projection 的兜底来源。
 */
bool state_model_path_source_allowed(const HiCacheFact & fact) {
    if (!fact.is_end || !fact.has_consumer("hicache_state_model")) return false;
    if (fact.fact_class == "target_policy_input") return fact.role == "prefetch_decision";
    if (fact.fact_class != "workload_identity") return false;
    return fact.role == "request_bound_match_anchor" || fact.role == "request_lifecycle_anchor" || fact.role == "request_admission";
}

std::string snapshot_id_for_fact(const HiCacheFact & fact, HiCacheTokenSnapshotStage stage) {
    if (fact.request_id.empty()) return "";
    return normalized_scope(fact) + ":" + fact.request_id + ":" + std::to_string(fact.source_event_index) + ":" + std::to_string(fact.seq_no) + ":"
           + hicache_token_snapshot_stage_name(stage);
}

bool snapshot_before_fact(const HiCacheTokenPathSnapshot & snapshot, const HiCacheFact & fact) {
    if (snapshot.seq_no != 0 && fact.seq_no != 0 && snapshot.seq_no != fact.seq_no) return snapshot.seq_no < fact.seq_no;
    if (snapshot.source_event_index != fact.source_event_index) return snapshot.source_event_index < fact.source_event_index;
    if (snapshot.ts != fact.ts) return snapshot.ts < fact.ts;
    return false;
}

bool committed_stage(HiCacheTokenSnapshotStage stage) {
    return stage == HiCacheTokenSnapshotStage::Admission || stage == HiCacheTokenSnapshotStage::LifecycleUnfinished
           || stage == HiCacheTokenSnapshotStage::LifecycleFinished;
}

bool snapshot_stage_observable(HiCacheTokenSnapshotStage stage) { return stage != HiCacheTokenSnapshotStage::Unknown; }

HiCacheTokenResolution missing_resolution(const HiCacheFact & fact, const std::string & reason) {
    return HiCacheTokenResolution{
        .status = HiCacheTokenResolutionStatus::Missing,
        .stage = stage_for_fact(fact),
        .token_count = fact.token_count,
        .reason = reason,
    };
}

HiCacheTokenResolution source_rejected_resolution(const HiCacheFact & fact) {
    return HiCacheTokenResolution{
        .status = HiCacheTokenResolutionStatus::SourceClassRejected,
        .stage = stage_for_fact(fact),
        .token_count = fact.token_count,
        .reason = "token path snapshot is not a state-model path fact",
    };
}

HiCacheTokenResolution wrong_stage_resolution(const HiCacheFact & fact, const std::string & reason) {
    return HiCacheTokenResolution{
        .status = HiCacheTokenResolutionStatus::WrongStageRejected,
        .stage = stage_for_fact(fact),
        .token_count = fact.full_path_span.valid ? fact.full_path_span.token_count : static_cast<uint64_t>(fact.full_path_tokens.size()),
        .reason = reason,
    };
}

HiCacheTokenResolution direct_fact_resolution(const HiCacheFact & fact, uint64_t page_size, HiCacheTokenSnapshotStage stage, std::string reason) {
    const auto token_count = fact.full_path_span.valid ? fact.full_path_span.token_count : static_cast<uint64_t>(fact.full_path_tokens.size());
    return HiCacheTokenResolution{
        .status = HiCacheTokenResolutionStatus::Direct,
        .tokens = fact.full_path_tokens,
        .snapshot_id = snapshot_id_for_fact(fact, stage),
        .stage = stage,
        .token_count = token_count,
        .page_aligned_token_count = aligned_token_count(token_count, page_size),
        .reason = std::move(reason),
    };
}

} // namespace token_store_detail

using token_store_detail::aligned_token_count;
using token_store_detail::committed_stage;
using token_store_detail::completeness_for_snapshot;
using token_store_detail::direct_fact_resolution;
using token_store_detail::lifecycle_stage_name;
using token_store_detail::missing_resolution;
using token_store_detail::normalized_scope;
using token_store_detail::rank;
using token_store_detail::snapshot_before_fact;
using token_store_detail::snapshot_id_for_fact;
using token_store_detail::snapshot_stage_observable;
using token_store_detail::source_rejected_resolution;
using token_store_detail::stage_for_fact;
using token_store_detail::state_model_path_source_allowed;
using token_store_detail::wrong_stage_resolution;

std::string hicache_token_snapshot_stage_name(HiCacheTokenSnapshotStage stage) {
    switch (stage) {
    case HiCacheTokenSnapshotStage::Unknown:
        return "unknown";
    case HiCacheTokenSnapshotStage::Match:
        return "match";
    case HiCacheTokenSnapshotStage::Admission:
        return "admission";
    case HiCacheTokenSnapshotStage::LifecycleUnfinished:
        return "lifecycle_unfinished";
    case HiCacheTokenSnapshotStage::LifecycleFinished:
        return "lifecycle_finished";
    case HiCacheTokenSnapshotStage::PrefetchCandidate:
        return "prefetch_candidate";
    case HiCacheTokenSnapshotStage::StorageReadable:
        return "storage_readable";
    }
    return "unknown";
}

std::string hicache_token_resolution_status_name(HiCacheTokenResolutionStatus status) {
    switch (status) {
    case HiCacheTokenResolutionStatus::Direct:
        return "direct";
    case HiCacheTokenResolutionStatus::TimelineFallback:
        return "timeline_fallback";
    case HiCacheTokenResolutionStatus::Missing:
        return "missing";
    case HiCacheTokenResolutionStatus::StaleRejected:
        return "stale_rejected";
    case HiCacheTokenResolutionStatus::WrongStageRejected:
        return "wrong_stage_rejected";
    case HiCacheTokenResolutionStatus::SourceClassRejected:
        return "source_class_rejected";
    }
    return "missing";
}

std::string HiCacheTokenDirectory::scoped_request_key(const HiCacheFact & fact) const {
    if (fact.request_id.empty()) return "";
    return normalized_scope(fact) + ":" + fact.request_id;
}

void HiCacheTokenDirectory::observe_fact_path(const HiCacheFact & fact, uint64_t page_size) {
    const auto key = scoped_request_key(fact);
    if (key.empty() || !hicache_fact_has_resolved_full_path(fact) || !state_model_path_source_allowed(fact)) return;

    /* snapshot 只记录当前 fact 明确携带的 path 语义阶段。prefetch candidate 不会覆盖
       admission/lifecycle committed path，避免把投机路径当成 request 已提交状态。 */
    const auto stage = stage_for_fact(fact);
    if (!snapshot_stage_observable(stage)) return;
    const auto token_count = static_cast<uint64_t>(fact.full_path_tokens.size());
    auto snapshot = HiCacheTokenPathSnapshot{
        .snapshot_id = snapshot_id_for_fact(fact, stage),
        .cache_scope = normalized_scope(fact),
        .request_id = fact.request_id,
        .source_event_index = fact.source_event_index,
        .seq_no = fact.seq_no,
        .ts = fact.ts,
        .role = fact.role,
        .stage = stage,
        .lifecycle_kind = fact.lifecycle_kind,
        .admission_kind = fact.admission_kind,
        .span = fact.full_path_span,
        .tokens = fact.full_path_tokens,
        .token_count = token_count,
        .page_aligned_token_count = aligned_token_count(token_count, page_size),
        .completeness = completeness_for_snapshot(fact, page_size),
    };

    snapshots_.push_back(std::move(snapshot));
    snapshots_by_request_[key].push_back(snapshots_.size() - 1);
}

HiCacheTokenResolution HiCacheTokenDirectory::resolve_match_path(const HiCacheFact & fact, uint64_t page_size) const {
    /* match/admission/lifecycle/prefetch resolver 都采用 fact-local direct resolution。
       早期 timeline fallback 已被移除，因为它会在合同缺 path 时静默复用旧阶段 path。 */
    if (!state_model_path_source_allowed(fact)) return source_rejected_resolution(fact);
    if (stage_for_fact(fact) != HiCacheTokenSnapshotStage::Match)
        return wrong_stage_resolution(fact, "match resolver only accepts request_bound_match_anchor facts");
    if (!hicache_fact_has_resolved_full_path(fact)) return missing_resolution(fact, "request_bound_match_anchor requires a fact-local lookup token path");
    return direct_fact_resolution(fact, page_size, HiCacheTokenSnapshotStage::Match, "match anchor consumes its own lookup path");
}

HiCacheTokenResolution HiCacheTokenDirectory::resolve_admission_path(const HiCacheFact & fact, uint64_t page_size) const {
    if (!state_model_path_source_allowed(fact)) return source_rejected_resolution(fact);
    if (stage_for_fact(fact) != HiCacheTokenSnapshotStage::Admission)
        return wrong_stage_resolution(fact, "admission resolver only accepts request_admission facts");
    if (!hicache_fact_has_resolved_full_path(fact)) return missing_resolution(fact, "request_admission requires a fact-local admission token path");
    return direct_fact_resolution(fact, page_size, HiCacheTokenSnapshotStage::Admission, "admission consumes its own admission path");
}

HiCacheTokenResolution HiCacheTokenDirectory::resolve_lifecycle_path(const HiCacheFact & fact, uint64_t page_size) const {
    if (!state_model_path_source_allowed(fact)) return source_rejected_resolution(fact);
    const auto expected_stage = stage_for_fact(fact);
    if (expected_stage != HiCacheTokenSnapshotStage::LifecycleFinished && expected_stage != HiCacheTokenSnapshotStage::LifecycleUnfinished) {
        return wrong_stage_resolution(fact, "request_lifecycle_anchor has an unsupported lifecycle kind");
    }
    if (!hicache_fact_has_resolved_full_path(fact)) return missing_resolution(fact, "request_lifecycle_anchor requires a fact-local committed lifecycle path");
    return direct_fact_resolution(fact,
                                  page_size,
                                  expected_stage,
                                  "lifecycle anchor consumes its own committed path for " + lifecycle_stage_name(fact.lifecycle_kind));
}

HiCacheTokenResolution HiCacheTokenDirectory::resolve_prefetch_path(const HiCacheFact & fact, uint64_t page_size) const {
    if (!state_model_path_source_allowed(fact)) return source_rejected_resolution(fact);
    if (stage_for_fact(fact) != HiCacheTokenSnapshotStage::PrefetchCandidate)
        return wrong_stage_resolution(fact, "prefetch resolver only accepts prefetch_decision facts");
    if (!hicache_fact_has_resolved_full_path(fact)) return missing_resolution(fact, "prefetch_decision requires a fact-local prefetch candidate path");
    return direct_fact_resolution(fact,
                                  page_size,
                                  HiCacheTokenSnapshotStage::PrefetchCandidate,
                                  "prefetch consumes its own candidate path without updating committed request path");
}

const HiCacheTokenPathSnapshot * HiCacheTokenDirectory::previous_committed_snapshot(const HiCacheFact & fact) const {
    /* 仅供诊断和增长检查使用。正常状态推进不应靠这个接口补齐当前 fact 的 path，
       否则会重新引入跨 role fallback。 */
    const auto key = scoped_request_key(fact);
    if (key.empty()) return nullptr;

    const auto it = snapshots_by_request_.find(key);
    if (it == snapshots_by_request_.end()) return nullptr;

    const HiCacheTokenPathSnapshot * best = nullptr;
    for (const auto index : it->second) {
        const auto & snapshot = snapshots_[index];
        if (!committed_stage(snapshot.stage)) continue;
        if (!snapshot_before_fact(snapshot, fact)) continue;
        if (best == nullptr) {
            best = &snapshot;
            continue;
        }
        const bool later_seq = snapshot.seq_no > best->seq_no;
        const bool same_seq_later_event = snapshot.seq_no == best->seq_no && snapshot.source_event_index > best->source_event_index;
        const bool better_same_time = snapshot.seq_no == best->seq_no && snapshot.source_event_index == best->source_event_index
                                      && rank(snapshot.completeness) > rank(best->completeness);
        if (later_seq || same_seq_later_event || better_same_time) best = &snapshot;
    }
    return best;
}

} // namespace markov::trace_graph::modules::hicache::runtime
