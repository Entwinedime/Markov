#pragma once

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace markov::trace_graph::structure {

/** Missing source semantics or target work rules, never a cost-acquisition
 * request. Details describe the encountered frontier, not full graph coverage. */
struct MissingWork : std::runtime_error {
    nlohmann::json details;

    MissingWork(const std::string & reason, nlohmann::json gaps) : std::runtime_error(reason), details(std::move(gaps)) {}
};

} // namespace markov::trace_graph::structure
