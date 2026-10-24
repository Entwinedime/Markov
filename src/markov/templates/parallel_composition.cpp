#include "markov/templates/parallel_composition.hpp"

#include <stdexcept>
#include <vector>

namespace markov {

ParallelComposition::ParallelComposition(RegionGraph & graph) : graph_(graph) {}

void ParallelComposition::require_geometry(std::span<const std::size_t> reference, std::span<const std::size_t> target) const {
    if (target.size() != reference.size()) throw std::invalid_argument("collective tensor counts differ between ranks");
    for (std::size_t value = 0; value < target.size(); ++value) {
        for (const auto * field : { "dtype", "device", "shape" }) {
            if (graph_.types[target[value]].at(field) != graph_.types[reference[value]].at(field))
                throw std::invalid_argument("collective tensor geometry differs between ranks");
        }
    }
}

void ParallelComposition::connect(std::span<const RankWork> ranks) {
    std::map<std::string, std::vector<std::vector<std::size_t>>> groups;
    for (std::size_t rank = 0; rank < ranks.size(); ++rank) {
        for (auto operation = ranks[rank].begin; operation < ranks[rank].end; ++operation) {
            const auto & work = graph_.work[operation];
            if (!work.contains("communication")) continue;
            const auto & spec = work.at("communication");
            if (spec.at("size").get<std::size_t>() != ranks.size()) throw std::invalid_argument("collective participant count differs from target ranks");
            auto & calls = groups.try_emplace(spec.at("group").get<std::string>(), ranks.size()).first->second;
            calls[rank].push_back(operation);
        }
    }

    for (const auto & [group, calls] : groups) {
        for (const auto & rank : calls) {
            if (rank.size() != calls.front().size()) throw std::invalid_argument("ranks have different collective call counts");
        }
        const auto [resource, added] = resources_.try_emplace(group, graph_.execution_resources.size());
        if (added) graph_.execution_resources.push_back("communication");

        for (std::size_t index = 0; index < calls.front().size(); ++index) {
            std::vector<std::size_t> operations;
            std::vector<std::size_t> inputs;
            std::vector<std::size_t> outputs;
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
                inputs.insert(inputs.end(), access.inputs.begin(), access.inputs.end());
                outputs.insert(outputs.end(), access.outputs.begin(), access.outputs.end());
            }

            const auto region = graph_.regions.size();
            graph_.regions.push_back({
                {              "id",        region },
                {          "parent",       nullptr },
                {            "role",  "collective" },
                {   "communication", specification },
                { "rank_operations",    operations },
                {          "inputs",        inputs },
                {         "outputs",       outputs }
            });
            graph_.execution.connect_communication(operations, resource->second, region);
        }
    }
}

} // namespace markov
