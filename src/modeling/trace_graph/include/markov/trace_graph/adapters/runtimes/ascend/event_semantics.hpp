#pragma once

#include "markov/trace_graph/core/trace_event.hpp"

#include <array>
#include <string_view>

namespace markov::trace_graph::adapters::ascend {

/** Names removed only from torch-npu Level0's display export. Its original
 * CANN export still contains them, on the same profiler clock. */
inline constexpr std::array<std::string_view, 9> level0_pruned_event_prefixes{ "CANN", "AscendCL",   "Runtime",       "GE", "Node", "Model",
                                                                               "Hccl", "acl_to_npu", "Communication@" };

/** Interpret native API/profiler roles before source-window selection. Raw
 * names, arguments, provenance and timestamps remain unchanged. */
void classify_event(core::TraceEvent & event);

} // namespace markov::trace_graph::adapters::ascend
