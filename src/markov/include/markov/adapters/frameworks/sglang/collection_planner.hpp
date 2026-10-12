#pragma once

#include "markov/contracts/project.hpp"

#include <nlohmann/json_fwd.hpp>

#include <span>

namespace markov {

// Plan source configurations and bounded mechanism candidates.
// Neither grouping nor a recipe proves template coverage or runs collection.
class SGLangCollectionPlanner {
public:
    nlohmann::json configurations(std::span<const Candidate> candidates) const;

private:
    nlohmann::json candidate_recipe(const SGLangConfiguration & configuration) const;
};

} // namespace markov
