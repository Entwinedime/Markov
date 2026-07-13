/**
 * @file
 * @brief Effect-local boundary and no-double-count validation for shadow rewrites.
 */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/modules/hicache/patch/rewrite_transaction.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache::patch {

/** @brief Structural proof for one effect-local shadow rewrite. */
struct HiCacheBoundaryValidation {
    std::string effect_id;
    HiCacheRewriteKind rewrite_kind = HiCacheRewriteKind::Reject;
    bool ready = false;
    bool source_cost_removed = false;
    bool target_cost_materialized = false;
    bool ingress_preserved = false;
    bool egress_preserved = false;
    bool consumer_dependency_ready = false;
    std::string reason;
};

/** @brief Cell-wide boundary proof aggregate. */
struct HiCacheBoundaryValidationCatalog {
    std::string status = "not_built";
    std::vector<HiCacheBoundaryValidation> records;
    std::map<std::string, uint64_t> blocker_counts;

    [[nodiscard]] uint64_t ready_count() const;
};

/** @brief Validates retained boundaries and source/target cost ownership in one shadow plan. */
[[nodiscard]] HiCacheBoundaryValidationCatalog validate_hicache_shadow_boundaries(const core::DagGraph & graph, const HiCacheShadowRewriteTransaction & shadow);

} // namespace markov::trace_graph::modules::hicache::patch
