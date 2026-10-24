#pragma once

#include <cstddef>
#include <map>
#include <span>
#include <string>

#include "markov/graph/region_graph.hpp"

namespace markov {

struct RankWork {
    std::size_t begin;
    std::size_t end;
};

// Match collective interfaces in target program order, after each rank's batch has been assembled.
class ParallelComposition {
public:
    explicit ParallelComposition(RegionGraph & graph);
    void connect(std::span<const RankWork> ranks);

private:
    void require_geometry(std::span<const std::size_t> reference, std::span<const std::size_t> target) const;

    RegionGraph & graph_;
    std::map<std::string, std::size_t> resources_;
};

} // namespace markov
