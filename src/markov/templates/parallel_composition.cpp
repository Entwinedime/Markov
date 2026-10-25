#include "markov/templates/parallel_composition.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace markov {

ParallelComposition::ParallelComposition(RegionGraph & graph) : graph_(graph) {}

void ParallelComposition::require_geometry(std::span<const std::size_t> reference, std::span<const std::size_t> target) const {
    if (target.size() != reference.size()) throw std::invalid_argument("communication tensor counts differ between endpoints");
    for (std::size_t value = 0; value < target.size(); ++value) {
        for (const auto * field : { "dtype", "device", "shape" }) {
            if (graph_.types[target[value]].at(field) != graph_.types[reference[value]].at(field))
                throw std::invalid_argument("communication tensor geometry differs between endpoints");
        }
    }
}

void ParallelComposition::connect(std::span<const RankWork> ranks, const ParallelTopology & topology, const TaskIdentity & task) {
    std::map<std::string, std::vector<std::vector<std::size_t>>> groups;
    using MessageKey = std::tuple<std::string, std::size_t, std::size_t, std::string, std::string>;
    std::map<MessageKey, std::array<std::vector<std::size_t>, 2>> messages;
    for (const auto & rank : ranks) {
        for (auto operation = rank.begin; operation < rank.end; ++operation) {
            const auto & work = graph_.work[operation];
            if (!work.contains("communication")) continue;
            const auto & spec = work.at("communication");
            const auto & group = topology.group(rank.rank, spec.at("group").get<std::string>());
            if (spec.at("size").get<std::size_t>() != group.members.size())
                throw std::invalid_argument("communication participant count differs from its target group");

            const auto member = std::ranges::find(group.members, rank.rank) - group.members.begin();
            const auto primitive = spec.at("primitive").get<std::string>();
            if (primitive == "send" || primitive == "receive") {
                const auto count = static_cast<std::ptrdiff_t>(group.members.size());
                const auto offset = spec.at("offset").get<std::ptrdiff_t>();
                if (offset < -count || offset >= count) throw std::invalid_argument("message peer lies outside its bound group");
                const auto peer = group.members[(member + offset + count) % count];
                const bool sending = primitive == "send";
                const MessageKey key{ group.id,
                                      sending ? rank.rank : peer,
                                      sending ? peer : rank.rank,
                                      spec.at("channel").get<std::string>(),
                                      spec.at("field").get<std::string>() };
                messages[key][sending ? 0 : 1].push_back(operation);
                continue;
            }

            auto & calls = groups.try_emplace(group.id, group.members.size()).first->second;
            calls[static_cast<std::size_t>(member)].push_back(operation);
        }
    }

    for (const auto & [group, calls] : groups) {
        for (const auto & rank : calls) {
            if (rank.size() != calls.front().size()) throw std::invalid_argument("ranks have different collective call counts");
        }
        for (std::size_t index = 0; index < calls.front().size(); ++index) {
            std::vector<std::size_t> operations;
            const auto first = calls.front()[index];
            const auto & specification = graph_.work[first].at("communication");
            for (const auto & rank : calls) {
                const auto operation = rank[index];
                const auto & access = graph_.operations[operation];
                if (operation != first) {
                    if (graph_.work[operation].at("communication") != specification)
                        throw std::invalid_argument("ranks disagree on collective operation order");
                    require_geometry(graph_.operations[first].inputs, access.inputs);
                    require_geometry(graph_.operations[first].outputs, access.outputs);
                }
                operations.push_back(operation);
            }

            auto bound = specification;
            bound["group"] = group;
            connect_calls(operations, task, std::move(bound), index, "collective", {});
        }
    }

    // Match within a task, channel and tensor field; unrelated microbatches never share wire identity.
    for (const auto & [key, calls] : messages) {
        const auto & [group, source, target, channel, field] = key;
        if (calls[0].size() != calls[1].size()) throw std::invalid_argument("message has no matching send or receive endpoint");
        for (std::size_t index = 0; index < calls[0].size(); ++index) {
            const auto send = calls[0][index];
            const auto receive = calls[1][index];
            require_geometry(graph_.operations[send].inputs, graph_.operations[receive].outputs);
            const std::array operations{ send, receive };
            nlohmann::json specification{
                { "primitive", "send_receive" },
                {     "group",          group },
                {    "source",         source },
                {    "target",         target },
                {   "channel",        channel },
                {     "field",          field }
            };
            graph_.dependencies.push_back({ send, receive, "message" });
            connect_calls(operations, task, std::move(specification), index, "message", { source, target });
        }
    }
}

void ParallelComposition::connect_calls(std::span<const std::size_t> operations, const TaskIdentity & task, nlohmann::json specification, std::size_t call,
                                        const std::string & role, std::vector<std::size_t> link) {
    const auto key = std::pair{ specification.at("group").get<std::string>(), std::move(link) };
    const auto [resource, added] = resources_.try_emplace(key, graph_.execution_resources.size());
    if (added) graph_.execution_resources.push_back("communication");

    std::vector<std::size_t> inputs;
    std::vector<std::size_t> outputs;
    for (const auto operation : operations) {
        const auto & access = graph_.operations[operation];
        inputs.insert(inputs.end(), access.inputs.begin(), access.inputs.end());
        outputs.insert(outputs.end(), access.outputs.begin(), access.outputs.end());
    }

    const auto region = graph_.regions.size();
    graph_.regions.push_back({
        {              "id",                                                     region },
        {          "parent",                                                    nullptr },
        {            "role",                                                       role },
        {            "task", { { "group", task.group }, { "sequence", task.sequence } } },
        {            "call",                                                       call },
        {   "communication",                                   std::move(specification) },
        { "rank_operations",                                                 operations },
        {          "inputs",                                                     inputs },
        {         "outputs",                                                    outputs }
    });
    graph_.execution.connect_communication(operations, resource->second, region, role);
}

} // namespace markov
