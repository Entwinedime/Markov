#pragma once

#include "markov/trace_graph/planning/work.hpp"

#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>

namespace markov::adapters::ktransformers {

trace_graph::planning::Work phase_work(const nlohmann::json & configuration, const std::string & phase, std::size_t query, std::size_t context, bool first,
                                       bool emit);
trace_graph::planning::Work control_work(const nlohmann::json & configuration, std::size_t input, std::size_t prefills, std::size_t decodes, bool first);

} // namespace markov::adapters::ktransformers
