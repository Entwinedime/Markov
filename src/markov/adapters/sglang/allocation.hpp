#pragma once

#include "markov/trace_graph/planning/native_calls.hpp"

#include <memory>
#include <nlohmann/json.hpp>

namespace markov::adapters::sglang {

std::unique_ptr<const trace_graph::planning::CallSequence> read_decode_allocator(const nlohmann::json & args);

} // namespace markov::adapters::sglang
