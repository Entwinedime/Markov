#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/graph/region_graph.hpp"
#include "markov/templates/asset.hpp"

namespace markov {

struct Invocation {
    Parameters parameters;
    Parameters bindings;
    ParameterGroups input_groups;
    std::map<std::size_t, Invocation> calls;
    ParameterGroups sequences;

    static Invocation read(const nlohmann::json & source);
};

// Target values and calls are private to an expansion; shared asset definitions stay immutable.
class TemplateInstance {
public:
    static TemplateInstance select(const TemplateAsset & asset, const std::string & role, const std::string & phase, const Invocation & invocation);

    const std::vector<StateChange> & state_changes() const;
    const ValuePorts & ports() const;
    nlohmann::json document();

private:
    friend class TargetGraph;

    TemplateInstance(const TemplateAsset & asset, std::size_t layout, const FixedBindings & bindings, const Invocation & invocation, std::string role,
                     std::string phase);
    std::size_t expand(std::size_t layout, const FixedBindings & bindings, Invocation invocation, Parameters parameters, std::optional<std::size_t> parent,
                       const std::vector<std::size_t> & resources, std::vector<std::optional<std::size_t>> slots, GroupValues input_groups,
                       const ParameterGroups & sequences);
    std::size_t expand_call(const std::string & role, const Invocation & invocation, const Parameters & parameters, std::size_t parent,
                            const ValuePorts & ports, const ParameterGroups & sequences);
    void require_value(std::size_t value, const nlohmann::json & specification);

    // The task owns the compiled asset; instances own only their target values, calls and operations.
    const TemplateAsset & asset_;
    RegionGraph graph_;

    std::vector<std::size_t> inputs_;
    std::vector<std::size_t> outputs_;
    ValuePorts ports_;
    GroupValues group_inputs_;

    std::size_t layout_;
    std::string role_;
    std::string phase_;
};

} // namespace markov
