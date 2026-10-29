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

// Target parameters follow a declared role binding, independently of a source layout's call positions.
using ScopedParameters = std::map<std::string, std::map<std::int64_t, Parameters>>;

struct Invocation {
    Parameters parameters;
    Parameters bindings;
    ParameterGroups input_groups;
    std::map<std::size_t, Invocation> calls;
    ParameterGroups sequences;
    ScopedParameters parameter_scopes;

    static Invocation read(const nlohmann::json & source);
};

struct TargetInputs {
    const RegionGraph & graph;
    const std::map<std::string, std::size_t> & ports;
    const GroupValues & groups;
};

// Target values and calls are private to an expansion; shared asset definitions stay immutable.
class TemplateInstance {
public:
    static TemplateInstance select(const TemplateAsset & asset, const std::string & role, const std::string & phase, const Invocation & invocation,
                                   const TargetInputs * inputs = nullptr);

    const std::vector<StateChange> & state_changes() const;
    const ValuePorts & ports() const;
    nlohmann::json document();

private:
    friend class TargetGraph;

    TemplateInstance(const TemplateAsset & asset, std::size_t layout, const FixedBindings & bindings, const Invocation & invocation, std::string role,
                     std::string phase);
    std::size_t expand(std::size_t layout, const FixedBindings & bindings, Invocation invocation, Parameters parameters, std::optional<std::size_t> parent,
                       const std::vector<std::size_t> & resources, std::vector<std::optional<std::size_t>> slots, GroupValues input_groups,
                       const ParameterGroups & sequences, const ScopedParameters & scopes);
    std::size_t expand_call(const std::string & role, const Invocation & invocation, const Parameters & parameters, std::size_t parent,
                            const ValuePorts & ports, const ParameterGroups & sequences, const ScopedParameters & scopes);
    struct CallContext;
    void expand_operation(std::size_t operation, const TemplateDefinition & definition, const CallContext & context,
                          const std::vector<std::size_t> & own_values, const std::vector<std::size_t> & own_resources);
    void expand_child(const CallStep & call, std::size_t child, const CallContext & context, const std::vector<std::size_t> & values,
                      const std::vector<std::size_t> & resources);
    void require_value(std::size_t value, const nlohmann::json & specification);
    void bind_inputs(const TargetInputs & inputs);

    struct BoundInputs {
        std::map<std::size_t, std::size_t> values;
        std::vector<std::optional<std::size_t>> resources;
        std::map<std::size_t, nlohmann::json> types;
    };

    // The task owns the compiled asset; instances own only their target values, calls and operations.
    const TemplateAsset & asset_;
    RegionGraph graph_;

    std::vector<std::size_t> inputs_;
    std::vector<std::size_t> outputs_;
    ValuePorts ports_;
    GroupValues group_inputs_;
    BoundInputs bound_inputs_;

    std::size_t layout_;
    std::string role_;
    std::string phase_;
};

} // namespace markov
