#pragma once

#include <array>
#include <cstddef>
#include <deque>
#include <map>
#include <span>
#include <string>
#include <tuple>
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

// Collectives join one action; directed messages join the same flow even when peers arrive in separate actions.
class ParallelComposition {
public:
    explicit ParallelComposition(RegionGraph & graph);
    void connect(std::span<const RankWork> ranks, const ParallelTopology & topology, const TaskIdentity & task, std::size_t flow);
    void live_state(GraphRoots & roots);

private:
    void require_geometry(std::span<const std::size_t> reference, std::span<const std::size_t> target) const;
    nlohmann::json exchange(std::span<const std::size_t> operations) const;
    void connect_calls(std::span<const std::size_t> operations, const TaskIdentity & task, nlohmann::json specification, std::size_t call,
                       const std::string & role, std::vector<std::size_t> link);

    RegionGraph & graph_;
    std::map<std::pair<std::string, std::vector<std::size_t>>, std::size_t> resources_;

    using MessageKey = std::tuple<std::size_t, std::string, std::size_t, std::size_t, std::string, std::string>;
    std::map<MessageKey, std::array<std::deque<std::size_t>, 2>> messages_;
};

} // namespace markov
