/**
 * @file
 * @brief Internal HiCache per-effect cost assembly contract.
 */
#pragma once

#include "markov/trace_graph/cost/io_service.hpp"
#include "markov/trace_graph/modules/hicache/patch/io_resource_demand.hpp"

namespace markov::trace_graph::modules::hicache::patch::io_resource_model_detail {

[[nodiscard]] HiCacheIoCostRecord cost_record(const model::HiCacheEffectDecision & decision, const model::HiCacheEffectDecisionLedger & ledger,
                                              const frontend::HiCacheIoCostConfig & model_fields);

} // namespace markov::trace_graph::modules::hicache::patch::io_resource_model_detail
