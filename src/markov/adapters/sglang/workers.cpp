#include "markov/adapters/sglang/workers.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "markov/adapters/sglang/templates.hpp"

namespace markov::sglang {

WorkerBindings::WorkerBindings(const TemplateLibrary & library, const nlohmann::json & environment)
    : library_(library),
      source_(library.domain.at("worker_roles").at(std::to_string(library.resources.at(0).rank))),
      target_(library.domain.at("worker_roles").at(std::to_string(environment.at("parallel").at("rank").get<std::int64_t>()))) {
    if (source_.at("representative") != target_.at("representative")) throw std::invalid_argument("target worker has no source-declared equivalent role");
    auto domain = NativeTemplates::domain(environment);
    for (const auto name : { "parallel", "kv_layout", "expert_layers", "kv_peers" }) {
        if (!source_.contains(name)) continue;
        if (target_.at(name) != environment.at(name)) throw std::invalid_argument("target worker differs from its observed role binding");
        auto binding = source_.at(name);
        if (std::string_view(name) == "parallel") binding.erase("rank");
        if (binding != library.domain.at(name)) throw std::invalid_argument("source worker facts disagree with the template domain");
        domain[name] = std::move(binding);
    }

    // Placement changes require source evidence; all remaining implementation facts must still match.
    if (domain != library.domain) throw std::invalid_argument("action provider requires a matching implementation domain");
    const auto & source_sites = source_.at("modules");
    const auto & target_sites = target_.at("modules");
    if (source_sites.size() != target_sites.size()) throw std::invalid_argument("equivalent workers require corresponding native module positions");
    for (std::size_t index = 0; index < source_sites.size(); ++index) {
        if (!sites_.emplace(source_sites.at(index).get<std::string>(), target_sites.at(index).get<std::string>()).second)
            throw std::invalid_argument("native worker contains duplicate module bindings");
    }
    bind_pool("local", source_.at("kv_layout"), target_.at("kv_layout"));
    if (source_.contains("kv_peers")) {
        for (const auto & [name, peer] : source_.at("kv_peers").items()) bind_pool(name, peer.at("layout"), target_.at("kv_peers").at(name).at("layout"));
    }
}

void WorkerBindings::bind_pool(const std::string & name, const nlohmann::json & source, const nlohmann::json & target) {
    const auto source_begin = source.at("start_layer").get<std::int64_t>();
    const auto source_end = source.at("end_layer").get<std::int64_t>();
    const auto target_begin = target.at("start_layer").get<std::int64_t>();
    const auto target_end = target.at("end_layer").get<std::int64_t>();
    if (source_end - source_begin != target_end - target_begin) throw std::invalid_argument("equivalent workers require equal native pool layer counts");
    auto & layers = layers_[name];
    for (auto layer = source_begin; layer < source_end; ++layer) layers.emplace(layer, target_begin + (layer - source_begin));
}

void WorkerBindings::bind(TemplateFrame & frame, const std::string & instance) const {
    if (const auto site = sites_.find(frame.site); site != sites_.end()) frame.site = site->second;
    const auto & accesses = library_.definitions[frame.definition].kv_accesses;

    // Local and remote pools each use their own initialized layer correspondence.
    for (std::size_t index = 0; index < accesses.size(); ++index) { frame.kv_layers[index] = layers_.at(accesses[index].pool).at(frame.kv_layers[index]); }
    if (frame.communication) {
        auto & communication = *frame.communication;
        communication.rank = target_.at("parallel").at("rank").get<std::int64_t>();
        if (communication.operation != "transfer") {
            const auto & role = frame.bindings.at("collective_group").get_ref<const std::string &>();
            const auto & source = source_.at("groups").at(role);
            const auto & target = target_.at("groups").at(role);
            const auto members = target.at("members").get<std::vector<std::int64_t>>();
            if (source.at("members") != communication.members || source.at("channel") != communication.channel || source.at("role") != target.at("role")
                || members.size() != communication.members.size())
                throw std::invalid_argument("collective template disagrees with its native group binding");

            // A root is an index in the actual group, not a global rank offset.
            for (auto & rank : communication.required) {
                const auto position = std::ranges::find(communication.members, rank);
                if (position == communication.members.end()) throw std::invalid_argument("collective requirement is outside its native group");
                rank = members.at(static_cast<std::size_t>(position - communication.members.begin()));
            }
            communication.members = members;
            communication.channel = instance + "/" + target.at("channel").get<std::string>();
        }
    }
    for (auto & child : frame.children) bind(child, instance);
}

} // namespace markov::sglang
