#pragma once

#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace markov {

struct RankPlacement {
    std::string host;
    std::size_t device;
};

struct RankGroup {
    std::string id;
    std::string role;
    std::vector<std::size_t> members;
};

// Native group roles resolve within each logical rank; target group identities qualify shared resources.
class ParallelTopology {
public:
    ParallelTopology(std::vector<RankPlacement> ranks, std::vector<RankGroup> groups);

    std::span<const RankPlacement> ranks() const;
    std::span<const RankGroup> groups() const;
    const RankGroup & group(const std::string & identity) const;
    const RankGroup & group(std::size_t rank, const std::string & role) const;
    nlohmann::json document() const;

private:
    std::vector<RankPlacement> ranks_;
    std::vector<RankGroup> groups_;
    std::map<std::string, std::size_t> identities_;
    std::map<std::pair<std::size_t, std::string>, std::size_t> memberships_;
};

} // namespace markov
