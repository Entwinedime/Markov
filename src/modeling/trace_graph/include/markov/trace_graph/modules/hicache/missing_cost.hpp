#pragma once

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace markov::trace_graph::modules::hicache {

// A legal target operation lacks measured or justified extrapolated cost.
// Structural/dependency failures remain ordinary exceptions, not calibration requests.
struct MissingCostEvidence : std::runtime_error {
    nlohmann::json requirement;
    MissingCostEvidence(const std::string & component, nlohmann::json coordinates, const std::string & reason)
        : std::runtime_error(reason),
          requirement{
              {   "component",              component },
              { "coordinates", std::move(coordinates) },
              {      "reason",                 reason }
    } {}
};

} // namespace markov::trace_graph::modules::hicache
