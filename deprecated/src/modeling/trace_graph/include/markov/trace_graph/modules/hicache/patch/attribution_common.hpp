/**
 * @file
 * @brief Shared exact-ledger attribution helpers.
 */
#pragma once

#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/modules/hicache/patch/attribution.hpp"
#include "markov/trace_graph/source_dag/index.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace markov::trace_graph::modules::hicache::patch::attribution_detail {

[[nodiscard]] bool ledger_record_ready(const HiCacheIoOperationRecord & record);
void sort_unique(std::vector<size_t> & values);
[[nodiscard]] uint64_t fact_end(const adapters::sglang::HiCacheFactObservation & fact);
[[nodiscard]] uint64_t fact_boundary(const adapters::sglang::HiCacheFactObservation & fact);
[[nodiscard]] bool fact_precedes(const adapters::sglang::HiCacheFactObservation & left, const adapters::sglang::HiCacheFactObservation & right);
[[nodiscard]] uint64_t target_effective_token_count(const model::HiCacheEffectDecision & decision);

void append_snapshot_isolated_control_ownership(const adapters::sglang::HiCacheSourceIndex & source, const source_dag::TimingIntervalOwnership & ownership,
                                                std::string_view pid, std::string_view tid, HiCacheSourceAttribution & output);
void finalize_source_control_ownership(HiCacheSourceAttribution & output);
void assign_carrier_nodes(const adapters::sglang::HiCacheSourceIndex & source, std::vector<size_t> carrier_nodes, std::string reason,
                          HiCacheSourceAttribution & output);
void copy_completion_wait_contract(const HiCacheIoOperationRecord & operation, HiCacheSourceAttribution & output);
void classify_io_from_ledger(const adapters::sglang::HiCacheSourceIndex & source, const HiCacheIoOperationLedger & operations,
                             const model::HiCacheEffectDecision & decision, const adapters::sglang::HiCacheFactObservation & anchor,
                             HiCacheSourceAttribution & output);
[[nodiscard]] bool role_contract_available(const adapters::sglang::HiCacheSourceIndex & source, std::string_view role);
[[nodiscard]] std::vector<const adapters::sglang::HiCacheFactObservation *> commit_d2h_enqueues(const adapters::sglang::HiCacheSourceIndex & source,
                                                                                                const adapters::sglang::HiCacheFactObservation & anchor);
[[nodiscard]] std::vector<const adapters::sglang::HiCacheFactObservation *> capacity_release_calls(const adapters::sglang::HiCacheSourceIndex & source,
                                                                                                   const adapters::sglang::HiCacheFactObservation & anchor);
[[nodiscard]] bool all_capacity_releases_in_tail(const adapters::sglang::HiCacheSourceIndex & source, const adapters::sglang::HiCacheFactObservation & anchor);
void classify_commit_d2h_from_ledger(const adapters::sglang::HiCacheSourceIndex & source, const HiCacheIoOperationLedger & operations,
                                     const model::HiCacheEffectDecision & decision, const adapters::sglang::HiCacheFactObservation & anchor,
                                     HiCacheSourceAttribution & output);
void classify_commit_h2s_from_ledger(const adapters::sglang::HiCacheSourceIndex & source, const HiCacheIoOperationLedger & operations,
                                     const adapters::sglang::HiCacheFactObservation & anchor, HiCacheSourceAttribution & output);

} // namespace markov::trace_graph::modules::hicache::patch::attribution_detail
