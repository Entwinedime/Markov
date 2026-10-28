#pragma once

#include "markov/trace_graph/planning/native_calls.hpp"

#include <string>

namespace markov::adapters::sglang {

trace_graph::planning::KernelPreparation triton_preparation(const std::string & family, trace_graph::planning::Features specialization,
                                                            trace_graph::planning::Features launcher);

} // namespace markov::adapters::sglang
