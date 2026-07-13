/**
 * @file
 * @brief Post-apply semantic validation for one materialized HiCache patch transaction.
 */
#pragma once

#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/modules/hicache/patch/io_resource_model.hpp"
#include "markov/trace_graph/modules/hicache/patch/rewrite_transaction.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache::patch {

/** @brief Materialized invariants for one effect rewrite. */
struct HiCacheAppliedEffectValidation {
    std::string effect_id;
    HiCacheRewriteKind rewrite_kind = HiCacheRewriteKind::Reject;
    bool ready = false;
    bool source_duration_exact = false;
    bool synthetic_cost_exact = false;
    bool ingress_exact = false;
    bool consumer_dependency_exact = false;
    std::string reason;
};

/** @brief Cell-wide proof that the active graph matches the prospective patch plan. */
struct HiCacheAppliedPatchValidation {
    std::string status = "not_applied";
    bool plan_journal_exact = false;
    bool prospective_materialization_exact = false;
    bool topology_exact = false;
    bool family_dependencies_exact = false;
    bool lane_dependencies_exact = false;
    std::vector<HiCacheAppliedEffectValidation> records;
    std::map<std::string, uint64_t> blocker_counts;

    [[nodiscard]] uint64_t ready_count() const;
};

/**
 * @brief Verifies the concrete graph without rescanning the full node or edge arrays.
 *
 * The validator resolves synthetic nodes from the mutation journal, checks every planned
 * local mutation by stable node or edge index, and independently reconstructs expected
 * family and resource-lane dependencies from the rewrite and resource contracts.
 */
[[nodiscard]] HiCacheAppliedPatchValidation validate_hicache_applied_patch(const core::DagGraph & graph, const HiCacheShadowRewriteTransaction & shadow,
                                                                           const HiCacheIoResourcePlan & resources, const core::DagMutationJournal & journal,
                                                                           bool materialized_topology_valid);

} // namespace markov::trace_graph::modules::hicache::patch
