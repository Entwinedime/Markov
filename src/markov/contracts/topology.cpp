#include "markov/contracts/topology.hpp"

#include <stdexcept>
#include <utility>

namespace markov {

ParallelTopology::ParallelTopology(std::vector<RankPlacement> ranks, std::vector<RankGroup> groups) : ranks_(std::move(ranks)), groups_(std::move(groups)) {
    if (ranks_.empty() || groups_.empty()) throw std::invalid_argument("parallel topology needs ranks and groups");

    for (std::size_t index = 0; index < groups_.size(); ++index) {
        const auto & group = groups_[index];
        if (group.id.empty() || group.role.empty() || group.members.empty() || !identities_.try_emplace(group.id, index).second)
            throw std::invalid_argument("parallel groups need unique IDs, roles and members");

        // A role has one meaning at a rank, even when other groups overlap its membership.
        for (const auto rank : group.members) {
            if (rank >= ranks_.size() || !memberships_.try_emplace(std::pair{ rank, group.role }, index).second)
                throw std::invalid_argument("group members are outside the topology, duplicated or ambiguous for their role");
        }
    }
}

std::span<const RankPlacement> ParallelTopology::ranks() const { return ranks_; }
std::span<const RankGroup> ParallelTopology::groups() const { return groups_; }
const RankGroup & ParallelTopology::group(const std::string & identity) const { return groups_.at(identities_.at(identity)); }

const RankGroup & ParallelTopology::group(std::size_t rank, const std::string & role) const { return groups_.at(memberships_.at({ rank, role })); }

nlohmann::json ParallelTopology::document() const {
    nlohmann::json result{
        {  "ranks", nlohmann::json::array() },
        { "groups", nlohmann::json::array() }
    };
    for (std::size_t rank = 0; rank < ranks_.size(); ++rank) {
        result["ranks"].push_back({
            {   "rank",                rank },
            {   "host",   ranks_[rank].host },
            { "device", ranks_[rank].device }
        });
    }

    for (const auto & group : groups_) {
        result["groups"].push_back({
            {      "id",      group.id },
            {    "role",    group.role },
            { "members", group.members }
        });
    }

    return result;
}

} // namespace markov
