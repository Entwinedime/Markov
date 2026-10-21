#pragma once

#include "markov/contracts/control.hpp"
#include "markov/performance/cost_query.hpp"
#include "markov/performance/node_costs.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <map>
#include <memory>
#include <string>

namespace markov {

// Execution and coverage consume one query result. Sampling keeps complete gaps;
// full covered work is already recorded by the optional execution diagnostics.
class CostRequirements {
public:
    void record(const CostQuery & query, const NodeCostResult & measured, const InvokeStep & invocation, std::shared_ptr<const nlohmann::json> & origin);
    nlohmann::json summary() const;
    nlohmann::json data() const;

private:
    struct Requirement {
        std::shared_ptr<const nlohmann::json> origin;
        std::string coverage;
        std::string reason;
        std::size_t actual_services = 0;
    };

    std::map<CostQuery, Requirement> requirements_;
};

} // namespace markov
