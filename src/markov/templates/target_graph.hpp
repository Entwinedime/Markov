#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/templates/instance.hpp"

namespace markov {

struct TargetRegion {
    std::size_t region;
    ValuePorts ports;
    GroupValues group_inputs;
};

// Rank builders share target identities; source state and execution tokens stay in their rank's namespace.
class TargetGraph {
public:
    TargetGraph(const TemplateAsset & asset, RegionGraph & graph, std::size_t execution_offset = 0);

    TargetRegion append(const std::string & role, const std::string & phase, const Invocation & invocation,
                        const std::map<std::string, std::size_t> & inputs = {}, const GroupValues & groups = {});

    // Adapters compose target child calls inside an explicitly declared source host region.
    std::size_t begin_composition(const std::string & role, const std::string & phase, const Parameters & parameters,
                                  const std::map<std::string, std::size_t> & inputs);
    void end_composition(const std::map<std::string, std::size_t> & outputs);
    std::optional<std::size_t> composition() const;

    // Known contents do not remove a value's producer, transfers or storage dependencies.
    void bind_integers(std::size_t value, IntegerData data);
    const IntegerData & integers(std::size_t value) const;
    void live_state(GraphRoots & roots);

    const RegionGraph & graph() const;
    nlohmann::json document() const;

private:
    struct HostRegion {
        std::size_t identity;
        std::size_t resource;
    };

    const TemplateAsset & asset_;
    RegionGraph & graph_;
    std::size_t execution_offset_;
    std::map<std::size_t, std::size_t> external_resources_;
    std::vector<HostRegion> compositions_;
};

} // namespace markov
