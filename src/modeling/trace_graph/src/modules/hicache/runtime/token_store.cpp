/**
 * @file
 * @brief Fact-local HiCache token-path resolver and Debug history implementation.
 */
#include "markov/trace_graph/modules/hicache/runtime/token_store.hpp"

#include <utility>

namespace markov::trace_graph::modules::hicache::runtime {

namespace token_store_detail {


uint64_t aligned_token_count(uint64_t token_count, uint64_t page_size) {
    if (page_size == 0) return 0;
    return token_count / page_size * page_size;
}


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
using token_store_detail::direct_fact_resolution;
using token_store_detail::missing_resolution;
using token_store_detail::source_rejected_resolution;
using token_store_detail::state_model_path_source_allowed;
using token_store_detail::wrong_stage_resolution;


HiCacheTokenResolution HiCacheTokenDirectory::resolve_cache_lookup_path(const HiCacheFact & fact, uint64_t page_size) const {
    // Every resolver is fact-local. Timeline fallback was removed because it silently
    // carried a path from another semantic stage when the current contract was incomplete.
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


} // namespace markov::trace_graph::modules::hicache::runtime
