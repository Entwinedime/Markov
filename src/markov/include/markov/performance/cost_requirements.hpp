#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <map>
#include <string>

namespace markov {

struct CostQuery;
class NodeCosts;

// Only modeled physical services contribute performance requirements.
// Coverage and execution consume the same typed cost query.
class CostRequirements {
public:
    void record(const CostQuery & query, const NodeCosts & costs, const nlohmann::json & location, const nlohmann::json & work_origin);
    nlohmann::json describe() const;

private:
    struct Requirement {
        nlohmann::json object;
        nlohmann::json example;
        nlohmann::json work_origin;
        std::string coverage;
        std::string reason;
        std::size_t actual_services = 0;
        std::size_t measurement_required_services = 0;
        nlohmann::json measurement_example;
        nlohmann::json measurement_origin;
    };

    std::map<std::string, Requirement> requirements_;
};

} // namespace markov
