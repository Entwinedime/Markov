/**
 * @file
 * @brief Fact-local HiCache token-path resolver and Debug history implementation.
 */
#include "markov/trace_graph/modules/hicache/runtime/token_store.hpp"

#include <utility>

namespace markov::trace_graph::modules::hicache::runtime {

namespace token_store_detail {

#ifdef DEBUG
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
#endif

uint64_t aligned_token_count(uint64_t token_count, uint64_t page_size) {
    if (page_size == 0) return 0;
    return token_count / page_size * page_size;
}

#ifdef DEBUG
HiCacheTokenSnapshotStage stage_for_fact(const HiCacheFact & fact) {
    if (fact.role == "cache_lookup_input") return HiCacheTokenSnapshotStage::CacheLookup;
    if (fact.role == "cache_extend_input") return HiCacheTokenSnapshotStage::CacheExtend;
    if (fact.role == "prefetch_candidate_anchor") return HiCacheTokenSnapshotStage::PrefetchCandidate;
    if (fact.role == "cache_lifecycle_commit") {
        if (fact.lifecycle_kind == "finished") return HiCacheTokenSnapshotStage::LifecycleFinished;
        if (fact.lifecycle_kind == "unfinished") return HiCacheTokenSnapshotStage::LifecycleUnfinished;
    }
    return HiCacheTokenSnapshotStage::Unknown;
}
#endif

/**
 * @brief Returns whether a fact is an allowed state-model path source.
 *
 * Only contract-approved state-model phases may provide target page identity. A more
 * complete source-actual/oracle path must never become a fallback source.
 */
bool state_model_path_source_allowed(const HiCacheFact & fact) {
    if (!fact.has_consumer("hicache_state_model")) return false;
    if (fact.fact_class != "workload_identity") return false;
    if (fact.role == "cache_extend_input") return fact.is_start;
    if (!fact.is_end) return false;
    return fact.role == "cache_lookup_input" || fact.role == "cache_lifecycle_commit" || fact.role == "prefetch_candidate_anchor";
}

#ifdef DEBUG
bool snapshot_before_fact(const HiCacheTokenPathSnapshot & snapshot, const HiCacheFact & fact) {
    if (snapshot.seq_no != 0 && fact.seq_no != 0 && snapshot.seq_no != fact.seq_no) return snapshot.seq_no < fact.seq_no;
    if (snapshot.source_event_index != fact.source_event_index) return snapshot.source_event_index < fact.source_event_index;
    if (snapshot.ts != fact.ts) return snapshot.ts < fact.ts;
    return false;
}

bool committed_stage(HiCacheTokenSnapshotStage stage) {
    return stage == HiCacheTokenSnapshotStage::CacheExtend || stage == HiCacheTokenSnapshotStage::LifecycleUnfinished
           || stage == HiCacheTokenSnapshotStage::LifecycleFinished;
}

bool snapshot_stage_observable(HiCacheTokenSnapshotStage stage) { return stage != HiCacheTokenSnapshotStage::Unknown; }
#endif

HiCacheTokenResolution missing_resolution(const HiCacheFact & fact) {
    return HiCacheTokenResolution{
        .status = HiCacheTokenResolutionStatus::Missing,
        .tokens = {},
        .token_count = fact.token_count,
        .page_aligned_token_count = 0,
    };
}

HiCacheTokenResolution source_rejected_resolution(const HiCacheFact & fact) {
    return HiCacheTokenResolution{
        .status = HiCacheTokenResolutionStatus::SourceClassRejected,
        .tokens = {},
        .token_count = fact.token_count,
        .page_aligned_token_count = 0,
    };
}

HiCacheBatchTokenResolution batch_source_rejected_resolution() {
    return HiCacheBatchTokenResolution{
        .status = HiCacheTokenResolutionStatus::SourceClassRejected,
        .entries = {},
    };
}

HiCacheTokenResolution wrong_stage_resolution(const HiCacheFact & fact) {
    return HiCacheTokenResolution{
        .status = HiCacheTokenResolutionStatus::WrongStageRejected,
        .tokens = {},
        .token_count = fact.full_path_span.valid ? fact.full_path_span.token_count : static_cast<uint64_t>(fact.full_path_tokens.size()),
        .page_aligned_token_count = 0,
    };
}

HiCacheTokenResolution direct_fact_resolution(const HiCacheFact & fact, uint64_t page_size) {
    const auto token_count = fact.full_path_span.valid ? fact.full_path_span.token_count : static_cast<uint64_t>(fact.full_path_tokens.size());
    return HiCacheTokenResolution{
        .status = HiCacheTokenResolutionStatus::Direct,
        .tokens = fact.full_path_tokens,
        .token_count = token_count,
        .page_aligned_token_count = aligned_token_count(token_count, page_size),
    };
}

} // namespace token_store_detail

using token_store_detail::aligned_token_count;
using token_store_detail::batch_source_rejected_resolution;
#ifdef DEBUG
using token_store_detail::committed_stage;
using token_store_detail::completeness_for_snapshot;
#endif
using token_store_detail::direct_fact_resolution;
using token_store_detail::missing_resolution;
#ifdef DEBUG
using token_store_detail::normalized_scope;
using token_store_detail::rank;
using token_store_detail::snapshot_before_fact;
using token_store_detail::snapshot_stage_observable;
#endif
using token_store_detail::source_rejected_resolution;
#ifdef DEBUG
using token_store_detail::stage_for_fact;
#endif
using token_store_detail::state_model_path_source_allowed;
using token_store_detail::wrong_stage_resolution;

#ifdef DEBUG
std::string hicache_token_resolution_status_name(HiCacheTokenResolutionStatus status) {
    switch (status) {
    case HiCacheTokenResolutionStatus::Direct:
        return "direct";
    case HiCacheTokenResolutionStatus::Missing:
        return "missing";
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
    if (!state_model_path_source_allowed(fact)) return;

    // Record only the semantic stage explicitly carried by the fact. Prefetch candidates
    // remain separate from committed extend/lifecycle history.
    const auto stage = stage_for_fact(fact);
    if (!snapshot_stage_observable(stage)) return;
    if (stage == HiCacheTokenSnapshotStage::CacheExtend) {
        for (const auto & entry : fact.batch_paths) {
            HiCacheFact entry_fact = fact;
            entry_fact.request_id = entry.request_id;
            entry_fact.full_path_span = entry.full_path_span;
            entry_fact.full_path_tokens = entry.full_path_tokens;
            entry_fact.token_count = entry.token_count;
            if (hicache_fact_has_resolved_full_path(entry_fact)) append_snapshot(entry_fact, stage, page_size);
        }
        return;
    }
    if (!hicache_fact_has_resolved_full_path(fact)) return;
    append_snapshot(fact, stage, page_size);
}

void HiCacheTokenDirectory::append_snapshot(const HiCacheFact & fact, HiCacheTokenSnapshotStage stage, uint64_t page_size) {
    const auto key = scoped_request_key(fact);
    if (key.empty()) return;
    const auto token_count = static_cast<uint64_t>(fact.full_path_tokens.size());
    auto snapshot = HiCacheTokenPathSnapshot{
        .source_event_index = fact.source_event_index,
        .seq_no = fact.seq_no,
        .ts = fact.ts,
        .stage = stage,
        .page_aligned_token_count = aligned_token_count(token_count, page_size),
        .completeness = completeness_for_snapshot(fact, page_size),
    };

    snapshots_.push_back(std::move(snapshot));
    snapshots_by_request_[key].push_back(snapshots_.size() - 1);
}
#endif

HiCacheTokenResolution HiCacheTokenDirectory::resolve_cache_lookup_path(const HiCacheFact & fact, uint64_t page_size) const {
    // Every resolver is fact-local. Timeline fallback was removed because it silently
    // reused a path from another semantic stage when the current contract was incomplete.
    if (!state_model_path_source_allowed(fact)) return source_rejected_resolution(fact);
    if (fact.role != "cache_lookup_input") return wrong_stage_resolution(fact);
    if (!hicache_fact_has_resolved_full_path(fact)) return missing_resolution(fact);
    return direct_fact_resolution(fact, page_size);
}

HiCacheBatchTokenResolution HiCacheTokenDirectory::resolve_cache_extend_paths(const HiCacheFact & fact, uint64_t page_size) const {
    if (!state_model_path_source_allowed(fact)) return batch_source_rejected_resolution();
    if (fact.role != "cache_extend_input") {
        return HiCacheBatchTokenResolution{
            .status = HiCacheTokenResolutionStatus::WrongStageRejected,
            .entries = {},
        };
    }
    if (fact.batch_paths.empty()) {
        return HiCacheBatchTokenResolution{
            .status = HiCacheTokenResolutionStatus::Missing,
            .entries = {},
        };
    }
    HiCacheBatchTokenResolution batch{
        .status = HiCacheTokenResolutionStatus::Direct,
        .entries = {},
    };
    batch.entries.reserve(fact.batch_paths.size());
    for (const auto & entry : fact.batch_paths) {
        HiCacheFact entry_fact = fact;
        entry_fact.request_id = entry.request_id;
        entry_fact.full_path_span = entry.full_path_span;
        entry_fact.full_path_tokens = entry.full_path_tokens;
        entry_fact.token_count = entry.token_count;
        if (!hicache_fact_has_resolved_full_path(entry_fact)) { batch.status = HiCacheTokenResolutionStatus::Missing; }
        batch.entries.push_back(direct_fact_resolution(entry_fact, page_size));
        if (!hicache_fact_has_resolved_full_path(entry_fact)) batch.entries.back().status = HiCacheTokenResolutionStatus::Missing;
    }
    return batch;
}

HiCacheTokenResolution HiCacheTokenDirectory::resolve_cache_lifecycle_commit_path(const HiCacheFact & fact, uint64_t page_size) const {
    if (!state_model_path_source_allowed(fact)) return source_rejected_resolution(fact);
    if (fact.role != "cache_lifecycle_commit" || (fact.lifecycle_kind != "finished" && fact.lifecycle_kind != "unfinished")) {
        return wrong_stage_resolution(fact);
    }
    if (!hicache_fact_has_resolved_full_path(fact)) return missing_resolution(fact);
    return direct_fact_resolution(fact, page_size);
}

HiCacheTokenResolution HiCacheTokenDirectory::resolve_prefetch_candidate_path(const HiCacheFact & fact, uint64_t page_size) const {
    if (!state_model_path_source_allowed(fact)) return source_rejected_resolution(fact);
    if (fact.role != "prefetch_candidate_anchor") return wrong_stage_resolution(fact);
    if (!hicache_fact_has_resolved_full_path(fact)) return missing_resolution(fact);
    return direct_fact_resolution(fact, page_size);
}

#ifdef DEBUG
const HiCacheTokenPathSnapshot * HiCacheTokenDirectory::previous_committed_snapshot(const HiCacheFact & fact) const {
    // Diagnostics may compare growth against prior committed snapshots. Normal state
    // transitions never use this interface to fill the current fact's path.
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
#endif

} // namespace markov::trace_graph::modules::hicache::runtime
