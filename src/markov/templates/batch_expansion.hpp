#pragma once

#include <cstddef>
#include <vector>

#include "markov/contracts/serving.hpp"
#include "markov/graph/region_graph.hpp"

namespace markov {

// Framework adapters translate controller snapshots into the shared target graph.
class BatchExpansion {
public:
    virtual ~BatchExpansion() = default;

    // Native occupancy and budget units accompany each controller decision.
    virtual GroupAdmission admission() const = 0;
    virtual BatchProgram expand(const ServingAction & action) = 0;

    // Idle work returns readiness ports without publishing tokens or changing request ownership.
    virtual std::vector<std::size_t> idle() = 0;
    virtual const RegionGraph & graph() const = 0;

    // Release historical IR while pinning live state and message bindings; node ports keep their global identities.
    virtual std::vector<std::size_t> retire() = 0;
};

} // namespace markov
