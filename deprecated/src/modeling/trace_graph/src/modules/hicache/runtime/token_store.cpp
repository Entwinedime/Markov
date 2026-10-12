/**
 * @file
 * @brief Fact-local HiCache token-path resolution.
 */
#include "markov/trace_graph/modules/hicache/runtime/token_store.hpp"

#include <string_view>

namespace markov::trace_graph::modules::hicache::runtime {
namespace {

/** Only approved workload inputs provide paths; source-actual and oracle facts are not fallbacks. */
bool state_model_path_source_allowed(const HiCacheFact & fact) {
    if (!fact.has_consumer("hicache_state_model") || fact.fact_class != "workload_identity") return false;
    // Derived decode actions reuse an already admitted extend path.
    if (fact.role == "cache_extend_input" || fact.role == "cache_decode_allocation") return fact.is_start;
    if (!fact.is_end) return false;
    return fact.role == "cache_lookup_input" || fact.role == "cache_lifecycle_commit" || fact.role == "prefetch_candidate_anchor";
}

uint64_t path_token_count(const HiCacheTokenSpan & span, const HiCacheTokenPath & tokens) {
    return span.valid ? span.token_count : static_cast<uint64_t>(tokens.size());
}

HiCacheTokenResolution path_resolution(const HiCacheTokenSpan & span, const HiCacheTokenPath & tokens) {
    return {
        .status = HiCacheTokenResolutionStatus::Direct,
        .tokens = tokens,
        .token_count = path_token_count(span, tokens),
    };
}

HiCacheTokenResolution resolve_scalar_path(const HiCacheFact & fact, std::string_view role) {
    if (!state_model_path_source_allowed(fact)) return { .status = HiCacheTokenResolutionStatus::SourceClassRejected, .token_count = fact.token_count };
    if (fact.role != role || (role == "cache_lifecycle_commit" && fact.lifecycle_kind != "finished" && fact.lifecycle_kind != "unfinished"))
        return { .status = HiCacheTokenResolutionStatus::WrongStageRejected, .token_count = path_token_count(fact.full_path_span, fact.full_path_tokens) };
    if (!hicache_token_path_resolved(fact.full_path_span, fact.full_path_tokens))
        return { .status = HiCacheTokenResolutionStatus::Missing, .token_count = fact.token_count };
    return path_resolution(fact.full_path_span, fact.full_path_tokens);
}

} // namespace

HiCacheTokenResolution resolve_cache_lookup_path(const HiCacheFact & fact) { return resolve_scalar_path(fact, "cache_lookup_input"); }

HiCacheTokenResolution resolve_cache_lifecycle_commit_path(const HiCacheFact & fact) { return resolve_scalar_path(fact, "cache_lifecycle_commit"); }

HiCacheTokenResolution resolve_prefetch_candidate_path(const HiCacheFact & fact) { return resolve_scalar_path(fact, "prefetch_candidate_anchor"); }

HiCacheBatchTokenResolution resolve_cache_extend_paths(const HiCacheFact & fact) {
    if (!state_model_path_source_allowed(fact)) return { .status = HiCacheTokenResolutionStatus::SourceClassRejected };
    if (fact.role != "cache_extend_input" && fact.role != "cache_decode_allocation") return { .status = HiCacheTokenResolutionStatus::WrongStageRejected };
    if (fact.batch_paths.empty()) return { .status = HiCacheTokenResolutionStatus::Missing };

    HiCacheBatchTokenResolution batch{ .status = HiCacheTokenResolutionStatus::Direct };
    batch.entries.reserve(fact.batch_paths.size());
    for (const auto & entry : fact.batch_paths) {
        // Inspect the selected path, not a copy of the entire fact and its batch.
        batch.entries.push_back(path_resolution(entry.full_path_span, entry.full_path_tokens));
        if (!hicache_token_path_resolved(entry.full_path_span, entry.full_path_tokens)) {
            batch.entries.back().status = HiCacheTokenResolutionStatus::Missing;
            batch.status = HiCacheTokenResolutionStatus::Missing;
        }
    }
    return batch;
}

} // namespace markov::trace_graph::modules::hicache::runtime
