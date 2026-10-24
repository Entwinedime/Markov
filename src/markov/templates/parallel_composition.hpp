#pragma once

#include <cstddef>
#include <map>
#include <span>
#include <string>

#include "markov/contracts/topology.hpp"
#include "markov/graph/region_graph.hpp"

namespace markov {

struct RankWork {
    std::size_t rank;
    std::size_t begin;
    std::size_t end;
};

// Match collective interfaces within their bound groups after local work has been assembled.
class ParallelComposition {
public:
    explicit ParallelComposition(RegionGraph & graph);
    void connect(std::span<const RankWork> ranks, const ParallelTopology & topology);

private:
    void require_geometry(std::span<const std::size_t> reference, std::span<const std::size_t> target) const;

    RegionGraph & graph_;
    std::map<std::string, std::size_t> resources_;
};

} // namespace markov
