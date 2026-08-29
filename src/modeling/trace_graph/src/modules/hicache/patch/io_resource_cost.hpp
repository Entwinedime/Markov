/**
 * @file
 * @brief Internal HiCache per-effect cost assembly contract.
 */
#pragma once

#include "io_resource_service.hpp"

namespace markov::trace_graph::modules::hicache::patch::io_resource_model_detail {

[[nodiscard]] HiCacheIoCostRecord cost_record(const model::HiCacheEffectDecision & decision, const model::HiCacheEffectDecisionLedger & ledger,
                                              const frontend::HiCacheIoCostConfig & model_fields);

} // namespace markov::trace_graph::modules::hicache::patch::io_resource_model_detail
