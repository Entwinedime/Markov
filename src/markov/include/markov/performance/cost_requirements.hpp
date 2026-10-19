#pragma once

#include "markov/performance/node_costs.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <map>
#include <string>

namespace markov {

// Only modeled physical services contribute performance requirements.
// Coverage consumes the execution query result, never a second model lookup.
class CostRequirements {
public:
    void record(const CostQuery & query, const NodeCostResult & measured, const nlohmann::json & location, const nlohmann::json & work_origin);
    nlohmann::json describe(bool diagnostics) const;

private:
    struct Requirement {
        nlohmann::json object;
        nlohmann::json example;
        nlohmann::json work_origin;
        std::string coverage;
        std::string reason;
        std::size_t actual_services = 0;
    };

    std::map<std::string, Requirement> requirements_;
};

} // namespace markov
