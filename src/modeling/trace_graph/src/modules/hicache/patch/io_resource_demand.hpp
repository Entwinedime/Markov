/**
 * @file
 * @brief Operation-family naming and resource-lane ordering.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/patch/io_resource_model.hpp"

#include <string>

namespace markov::trace_graph::modules::hicache::patch::io_resource_model_detail {

[[nodiscard]] std::string operation_kind(const model::HiCacheEffectDecision & decision);
void append_lane_dependencies(HiCacheIoResourcePlan & plan);

} // namespace markov::trace_graph::modules::hicache::patch::io_resource_model_detail
