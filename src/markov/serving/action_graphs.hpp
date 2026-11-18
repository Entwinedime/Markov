#pragma once

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "markov/contracts/serving.hpp"
#include "markov/graph/graph.hpp"

namespace markov {

struct TemplateInterval;

// Framework ABI binds actions and describes their results without exposing native implementation details.
class ActionGraphs {
public:
    virtual ~ActionGraphs() = default;

    // Origins, when requested, follow the instantiated work order and are resolved before execution.
    virtual CoarseGraph build(const ServingAction & action, const nlohmann::json & bindings,
                              std::vector<TemplateInterval> * origins) = 0;
    virtual std::map<std::string, TensorLayout> outputs(std::string_view action, const nlohmann::json & condition, const Parameters & parameters) const = 0;
    virtual std::set<std::string> retained_bindings() const = 0;
};

} // namespace markov
