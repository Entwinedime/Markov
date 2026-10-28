#pragma once

#include "markov/trace_graph/planning/native_calls.hpp"

#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace markov::adapters::sglang {

std::unique_ptr<const trace_graph::planning::CallSequence> read_request_table_write(const nlohmann::json & metadata);
std::unique_ptr<const trace_graph::planning::CallSequence> read_forward_inputs(const nlohmann::json & metadata, bool deterministic_sampling);
std::optional<bool> read_deterministic_sampling(const std::optional<std::filesystem::path> & configuration);
std::unique_ptr<const trace_graph::planning::CallSequence> sampling_positions(const std::string & sequence_dtype);

} // namespace markov::adapters::sglang
