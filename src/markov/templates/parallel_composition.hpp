#pragma once

#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "markov/contracts/serving.hpp"
#include "markov/contracts/topology.hpp"
#include "markov/graph/region_graph.hpp"

namespace markov {

struct RankWork {
    std::size_t rank;
    std::size_t begin;
    std::size_t end;
};

// Match collective and directed message interfaces within one logical task and its bound groups.
class ParallelComposition {
public:
    explicit ParallelComposition(RegionGraph & graph);
    void connect(std::span<const RankWork> ranks, const ParallelTopology & topology, const TaskIdentity & task);

private:
    void require_geometry(std::span<const std::size_t> reference, std::span<const std::size_t> target) const;
    void connect_calls(std::span<const std::size_t> operations, const TaskIdentity & task, nlohmann::json specification, std::size_t call,
                       const std::string & role, std::vector<std::size_t> link);

    RegionGraph & graph_;
    std::map<std::pair<std::string, std::vector<std::size_t>>, std::size_t> resources_;
};

} // namespace markov
