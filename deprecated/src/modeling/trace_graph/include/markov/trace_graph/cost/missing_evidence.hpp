#pragma once

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace markov::trace_graph::cost {

// A legal target operation lacks measured or justified extrapolated cost.
// Structural/dependency failures remain ordinary exceptions, not calibration requests.
struct MissingCostEvidence : std::runtime_error {
    nlohmann::json requirements;
    MissingCostEvidence(const std::string & component, nlohmann::json coordinates, const std::string & reason)
        : std::runtime_error(reason),
          requirements(nlohmann::json::array({
              { { "component", component }, { "coordinates", std::move(coordinates) }, { "reason", reason } }
    })) {}

    explicit MissingCostEvidence(nlohmann::json missing)
        : std::runtime_error("Independent costs are unavailable for the admitted operation batch"),
          requirements(std::move(missing)) {}
};

} // namespace markov::trace_graph::cost
