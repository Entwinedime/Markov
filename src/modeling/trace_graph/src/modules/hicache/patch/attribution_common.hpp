/**
 * @file
 * @brief Shared exact-ledger attribution helpers.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/patch/attribution.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace markov::trace_graph::modules::hicache::patch::attribution_detail {

[[nodiscard]] bool ledger_record_ready(const HiCacheIoOperationRecord & record);
void sort_unique(std::vector<size_t> & values);
[[nodiscard]] uint64_t fact_end(const HiCacheSourceFactNode & fact);
[[nodiscard]] uint64_t fact_boundary(const HiCacheSourceFactNode & fact);
[[nodiscard]] bool fact_precedes(const HiCacheSourceFactNode & left, const HiCacheSourceFactNode & right);
[[nodiscard]] uint64_t target_effective_token_count(const model::HiCacheEffectDecision & decision);

void append_snapshot_isolated_control_ownership(const HiCacheSourceDagIndex & source, const HiCacheTimingIntervalOwnership & ownership, std::string_view pid,
                                                std::string_view tid, HiCacheSourceAttribution & output);
void finalize_source_control_ownership(const HiCacheSourceDagIndex & source, HiCacheSourceAttribution & output);
void assign_carrier_nodes(const HiCacheSourceDagIndex & source, std::vector<size_t> carrier_nodes, std::string reason, HiCacheSourceAttribution & output);
void copy_completion_wait_contract(const HiCacheIoOperationRecord & operation, HiCacheSourceAttribution & output);
void classify_io_from_ledger(const HiCacheSourceDagIndex & source, const HiCacheIoOperationLedger & operations, const model::HiCacheEffectDecision & decision,
                             const HiCacheSourceFactNode & anchor, HiCacheSourceAttribution & output);
[[nodiscard]] bool role_contract_available(const HiCacheSourceDagIndex & source, std::string_view role);
[[nodiscard]] std::vector<const HiCacheSourceFactNode *> commit_d2h_enqueues(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & anchor);
[[nodiscard]] std::vector<const HiCacheSourceFactNode *> capacity_release_calls(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & anchor);
[[nodiscard]] bool all_capacity_releases_in_tail(const HiCacheSourceDagIndex & source, const HiCacheSourceFactNode & anchor);
void classify_commit_d2h_from_ledger(const HiCacheSourceDagIndex & source, const HiCacheIoOperationLedger & operations,
                                     const model::HiCacheEffectDecision & decision, const HiCacheSourceFactNode & anchor, HiCacheSourceAttribution & output);
void classify_commit_h2s_from_ledger(const HiCacheSourceDagIndex & source, const HiCacheIoOperationLedger & operations, const HiCacheSourceFactNode & anchor,
                                     HiCacheSourceAttribution & output);

} // namespace markov::trace_graph::modules::hicache::patch::attribution_detail
