#include "markov/adapters/frameworks/sglang/topology.hpp"

#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace markov::sglang {

ServingTopology::ServingTopology(std::span<const TemplateAsset> assets, std::size_t stages, std::span<const std::int64_t> devices)
    : stages_(stages),
      parallel_(build(assets, stages, devices)) {}

ParallelTopology ServingTopology::build(std::span<const TemplateAsset> assets, std::size_t stages, std::span<const std::int64_t> devices) {
    if (assets.empty() || stages == 0 || assets.size() % stages != 0) throw std::invalid_argument("PP stages must divide the source rank count");
    if (!devices.empty() && devices.size() != assets.size()) throw std::invalid_argument("placement needs one device per logical rank");
    const auto tp = assets.size() / stages;
    std::set<std::size_t> source_ranks;
    for (const auto & asset : assets) {
        const auto & environment = asset.environment();
        const auto rank = environment.at("rank").get<std::size_t>();
        if (environment.at("framework") != "sglang" || environment.at("logical_tp").get<std::size_t>() != tp || rank >= assets.size()
            || !source_ranks.insert(rank).second)
            throw std::invalid_argument("SGLang needs one source per target TP/PP rank");
        if (stages > 1
            && (environment.at("logical_pp").get<std::size_t>() != stages || environment.at("pp_rank").get<std::size_t>() != rank / tp
                || environment.at("tp_rank").get<std::size_t>() != rank % tp))
            throw std::invalid_argument("source pipeline coordinates differ from the target topology");
        if (stages == 1 && environment.contains("logical_pp") && environment.at("logical_pp") != 1)
            throw std::invalid_argument("pipeline sources require their recorded PP topology");
    }

    std::vector<RankPlacement> ranks;
    std::set<std::size_t> used;
    for (std::size_t rank = 0; rank < assets.size(); ++rank) {
        const auto device = devices.empty() ? static_cast<std::int64_t>(rank) : devices[rank];
        if (device < 0 || !used.insert(static_cast<std::size_t>(device)).second)
            throw std::invalid_argument("rank placement needs distinct nonnegative devices on one host");
        ranks.push_back({ "local", static_cast<std::size_t>(device) });
    }

    std::vector<std::size_t> members(assets.size());
    std::iota(members.begin(), members.end(), 0);
    std::vector<RankGroup> groups{
        { "model", "serving", std::move(members) }
    };
    for (std::size_t stage = 0; stage < stages; ++stage) {
        std::vector<std::size_t> local(tp);
        std::iota(local.begin(), local.end(), stage * tp);
        groups.push_back({ "tp-" + std::to_string(stage), "tp", std::move(local) });
    }
    if (stages > 1) {
        for (std::size_t tensor_rank = 0; tensor_rank < tp; ++tensor_rank) {
            std::vector<std::size_t> local;
            for (std::size_t stage = 0; stage < stages; ++stage) local.push_back(stage * tp + tensor_rank);
            groups.push_back({ "pp-" + std::to_string(tensor_rank), "pp", std::move(local) });
        }
    }
    return { std::move(ranks), std::move(groups) };
}

const ParallelTopology & ServingTopology::parallel() const { return parallel_; }
const RankGroup & ServingTopology::stage(std::size_t index) const { return parallel_.group(index * tensor_parallel_size(), "tp"); }
std::size_t ServingTopology::stage_count() const { return stages_; }
std::size_t ServingTopology::tensor_parallel_size() const { return parallel_.ranks().size() / stages_; }

} // namespace markov::sglang
