#include "markov/adapters/frameworks/sglang/topology.hpp"

#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace markov::sglang {

ServingTopology::ServingTopology(std::span<const TemplateAsset> assets, std::span<const std::int64_t> devices) : parallel_(build(assets, devices)) {}

ParallelTopology ServingTopology::build(std::span<const TemplateAsset> assets, std::span<const std::int64_t> devices) {
    if (assets.empty()) throw std::invalid_argument("serving topology requires rank sources");
    if (!devices.empty() && devices.size() != assets.size()) throw std::invalid_argument("placement needs one device per logical rank");
    std::map<std::string, std::vector<const TemplateAsset *>> instances;
    std::set<std::size_t> identities;
    for (const auto & asset : assets) {
        const auto & environment = asset.environment();
        const auto rank = environment.at("rank").get<std::size_t>();
        if (environment.at("framework") != "sglang" || rank >= assets.size() || !identities.insert(rank).second)
            throw std::invalid_argument("SGLang sources need distinct ranks within the target world");
        const auto mode = environment.at("implementation").at("disaggregation_mode").get<std::string>();
        if (mode != "null" && mode != "prefill" && mode != "decode") throw std::invalid_argument("unknown serving instance role");
        instances[mode == "null" ? "model" : mode].push_back(&asset);
    }
    if (instances.size() > 1 && (instances.size() != 2 || !instances.contains("prefill") || !instances.contains("decode")))
        throw std::invalid_argument("sources must describe one serving instance or a prefill/decode pair");

    std::vector<RankPlacement> ranks;
    std::set<std::int64_t> used;
    for (std::size_t rank = 0; rank < assets.size(); ++rank) {
        const auto device = devices.empty() ? static_cast<std::int64_t>(rank) : devices[rank];
        if (device < 0 || !used.insert(device).second) throw std::invalid_argument("rank placement needs distinct nonnegative devices on one host");
        ranks.push_back({ "local", static_cast<std::size_t>(device) });
    }

    std::vector<RankGroup> groups;
    for (const auto & [name, sources] : instances) {
        const auto & first = sources.front()->environment();
        const auto tp = first.at("logical_tp").get<std::size_t>();
        const auto pp = first.at("logical_pp").get<std::size_t>();
        const auto ep = first.at("logical_ep").get<std::size_t>();
        if (tp == 0 || pp == 0 || sources.size() % tp != 0 || sources.size() / tp != pp || ep == 0 || (ep > 1 && ep != tp))
            throw std::invalid_argument("source roles do not cover their native TP/PP/EP geometry");

        std::vector<std::size_t> members(sources.size());
        std::set<std::size_t> coordinates;
        for (const auto * source : sources) {
            const auto & environment = source->environment();
            const auto tensor = environment.at("tp_rank").get<std::size_t>();
            const auto stage = environment.at("pp_rank").get<std::size_t>();
            if (environment.at("logical_tp") != tp || environment.at("logical_pp") != pp || environment.at("logical_ep") != ep || tensor >= tp || stage >= pp
                || !coordinates.insert(stage * tp + tensor).second || (ep > 1 && environment.at("ep_rank") != tensor))
                throw std::invalid_argument("source coordinates disagree within a serving instance");
            members[stage * tp + tensor] = environment.at("rank").get<std::size_t>();
        }
        groups.push_back({ name, "serving", members });

        // Group identities include the instance; native group roles remain the same at each rank.
        for (std::size_t stage = 0; stage < pp; ++stage) {
            std::vector<std::size_t> local(members.begin() + stage * tp, members.begin() + (stage + 1) * tp);
            if (ep > 1) groups.push_back({ name + ".ep-" + std::to_string(stage), "ep", local });
            groups.push_back({ name + ".tp-" + std::to_string(stage), "tp", std::move(local) });
        }
        if (pp > 1) {
            for (std::size_t tensor = 0; tensor < tp; ++tensor) {
                std::vector<std::size_t> local;
                for (std::size_t stage = 0; stage < pp; ++stage) local.push_back(members[stage * tp + tensor]);
                groups.push_back({ name + ".pp-" + std::to_string(tensor), "pp", std::move(local) });
            }
        }
    }
    if (instances.size() == 2) {
        std::vector<std::size_t> members(assets.size());
        std::iota(members.begin(), members.end(), 0);
        groups.push_back({ "kv", "kv", std::move(members) });
    }
    return { std::move(ranks), std::move(groups) };
}

const ParallelTopology & ServingTopology::parallel() const { return parallel_; }
const RankGroup & ServingTopology::instance(const std::string & name) const { return parallel_.group(name); }
const RankGroup & ServingTopology::stage(const std::string & name, std::size_t index) const {
    return parallel_.group(instance(name).members.at(index * tensor_parallel_size(name)), "tp");
}
std::size_t ServingTopology::tensor_parallel_size(const std::string & name) const {
    return parallel_.group(instance(name).members.front(), "tp").members.size();
}
std::size_t ServingTopology::stage_count(const std::string & name) const { return instance(name).members.size() / tensor_parallel_size(name); }

} // namespace markov::sglang
