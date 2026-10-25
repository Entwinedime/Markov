#include "markov/adapters/frameworks/sglang/expert_routing.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace markov::sglang {

ExpertRouting::Geometry ExpertRouting::discover(std::span<const TemplateAsset> assets) {
    const auto & first = assets.front().environment();
    Geometry result{ first.at("logical_ep").get<std::int64_t>(),
                     first.at("implementation").at("model_config").at("num_hidden_layers").get<std::int64_t>(),
                     {},
                     {} };
    std::map<std::int64_t, ExpertLayer> layers;

    for (const auto & asset : assets) {
        const auto & environment = asset.environment();
        const auto rank = environment.at("rank").get<std::size_t>();
        if (result.size > 1 && environment.at("implementation").at("expert_transport") != "1")
            throw std::invalid_argument("expert routing requires the observed DeepEP all-to-all implementation");

        for (const auto & entry : asset.entrypoints()) {
            if (entry.role != "expert_layer") continue;
            const auto & bindings = asset.frame(entry).bindings.values;
            const ExpertLayer layer{ bindings.at("expert_layer"), bindings.at("expert_count"), bindings.at("expert_top_k") };
            const Partition partition{ bindings.at("expert_rank"), bindings.at("expert_begin"), bindings.at("local_expert_count"), layer.experts };
            if (layer.id < 0 || layer.id >= result.model_layers || layer.experts <= 0 || layer.top_k <= 0 || layer.top_k > layer.experts || result.size <= 0
                || layer.experts % result.size != 0 || partition.count != layer.experts / result.size || partition.rank < 0 || partition.rank >= result.size
                || partition.rank != environment.at("ep_rank").get<std::int64_t>() || partition.begin != partition.rank * partition.count)
                throw std::invalid_argument("native expert ownership is inconsistent");

            const auto [position, added] = layers.emplace(layer.id, layer);
            if (!added && (position->second.experts != layer.experts || position->second.top_k != layer.top_k))
                throw std::invalid_argument("expert layer geometry differs between source roles");
            result.ranks[rank].insert_or_assign(layer.id, partition);
        }
    }
    for (const auto & [_, layer] : layers) result.layers.push_back(layer);
    return result;
}

ExpertRouting::ExpertRouting(std::span<const TemplateAsset> assets, std::span<const Request> requests, const nlohmann::json & source)
    : geometry_(discover(assets)),
      routes_(source, requests, geometry_.layers, geometry_.model_layers, geometry_.size - 1) {}

std::vector<std::int64_t> ExpertRouting::origin_counts(std::int64_t layer, std::int64_t experts, const Batch & batch, std::int64_t rank) const {
    const auto tokens = batch.new_tokens();
    const auto extra = (geometry_.size - tokens % geometry_.size) % geometry_.size;
    if (tokens > std::numeric_limits<std::int64_t>::max() - extra) throw std::invalid_argument("expert padding overflows the token range");
    const auto local_rows = (tokens + extra) / geometry_.size;
    const auto first = rank * local_rows;
    const auto last = first + local_rows;
    std::vector<std::int64_t> result(static_cast<std::size_t>(experts));

    // This backend pads the complete token vector before splitting contiguous attention-TP slices.
    std::int64_t offset = 0;
    for (const auto & member : batch.members) {
        const auto begin = std::max(first, offset);
        const auto end = std::min(last, offset + member.new_tokens);
        if (begin < end) {
            const auto counts = routes_.counts(member.request, layer, member.context_tokens + begin - offset, member.context_tokens + end - offset);
            for (std::size_t expert = 0; expert < result.size(); ++expert) result[expert] += counts[expert];
        }
        offset += member.new_tokens;
    }
    if (last > tokens) {
        const auto counts = routes_.padding_counts(layer, std::max(first, tokens) - tokens, last - tokens);
        for (std::size_t expert = 0; expert < result.size(); ++expert) result[expert] += counts[expert];
    }
    return result;
}

void ExpertRouting::bind(Invocation & invocation, const Batch & batch, std::size_t rank) const {
    auto & scoped = invocation.parameter_scopes["expert_layer"];
    for (const auto & [layer, partition] : geometry_.ranks.at(rank)) {
        std::vector<std::vector<std::int64_t>> origins;
        for (std::int64_t peer = 0; peer < geometry_.size; ++peer) origins.push_back(origin_counts(layer, partition.experts, batch, peer));
        const auto & local = origins[static_cast<std::size_t>(partition.rank)];
        auto & parameters = scoped[layer];
        std::int64_t received = 0;

        for (std::int64_t peer = 0; peer < geometry_.size; ++peer) {
            const auto begin = static_cast<std::size_t>(peer * partition.count);
            parameters["expert_send_" + std::to_string(peer)] =
                std::accumulate(local.begin() + begin, local.begin() + begin + partition.count, std::int64_t{ 0 });
            const auto & source = origins[static_cast<std::size_t>(peer)];
            const auto count = std::accumulate(source.begin() + partition.begin, source.begin() + partition.begin + partition.count, std::int64_t{ 0 });
            parameters["expert_receive_" + std::to_string(peer)] = count;
            received += count;
        }
        parameters["expert_received"] = received;

        for (std::int64_t expert = 0; expert < partition.count; ++expert) {
            std::int64_t count = 0;
            for (const auto & source : origins) count += source[static_cast<std::size_t>(partition.begin + expert)];
            parameters["expert_" + std::to_string(expert)] = count;
        }
    }
}

} // namespace markov::sglang
