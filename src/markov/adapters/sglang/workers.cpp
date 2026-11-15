#include "markov/adapters/sglang/workers.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
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

    std::vector<const TemplateFrame *> frames;
    for (const auto & entry : library.entries) frames.push_back(&entry.frame);
    for (std::size_t index = 0; index < frames.size(); ++index) {
        const auto & frame = *frames[index];
        for (const auto & child : frame.children) frames.push_back(&child);
        if (!frame.state_input) continue;
        const auto & body = library.definitions[frame.definition];
        std::set<std::string> peers;
        for (std::size_t access = 0; access < body.kv_accesses.size(); ++access) {
            if (body.kv_accesses[access].access == StateAccess::Write) peers.insert(frame.state.kv[access].pool);
        }
        for (std::size_t access = 0; access < body.slot_accesses.size(); ++access) {
            if (body.slot_accesses[access].access == StateAccess::Write) peers.insert(frame.state.slots[access]);
        }
        if (peers.size() != 1) throw std::invalid_argument("native transport binding requires one observed destination");
        const auto kind = frame.state_input->substr(0, frame.state_input->find('/'));
        const auto [found, inserted] = states_[kind].emplace(*peers.begin(), frame.state);
        if (!inserted && found->second != frame.state)
            throw std::invalid_argument("native transport destination has conflicting observed geometry: " + *peers.begin());
    }
}

std::map<std::string, StateBindings> WorkerBindings::state_inputs(const nlohmann::json & bindings, const nlohmann::json & work) const {
    std::map<std::string, StateBindings> result;
    if (!bindings.is_object()) throw std::invalid_argument("native invocation bindings require an object");
    if (bindings.empty()) return result;
    if (bindings.size() != 1) throw std::invalid_argument("native graph received unknown invocation bindings");
    const auto order = bindings.at("transport_order").get<std::vector<std::string>>();
    std::map<std::string, std::string> destinations;
    for (const auto & [name, owner] : work.at("pools").get<std::map<std::string, std::string>>()) {
        if (!destinations.emplace(owner, name).second) throw std::invalid_argument("native transport repeats a destination owner");
    }
    const std::set<std::string> unique(order.begin(), order.end());
    if (unique.size() != order.size() || order.size() != destinations.size())
        throw std::invalid_argument("native transport order does not cover each destination once");

    // Geometry comes from captured endpoint facts; only the invocation's iteration order is dynamic.
    for (const auto & [kind, peers] : states_) {
        for (std::size_t index = 0; index < order.size(); ++index) {
            auto state = peers.at(destinations.at(order[index]));
            for (auto & value : state.kv) value.layer = layers_.at(value.pool).at(value.layer);
            result.emplace(kind + "/" + std::to_string(index), std::move(state));
        }
    }
    return result;
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

void WorkerBindings::bind(TemplateFrame & frame, const std::string & instance, const nlohmann::json & work) const {
    bind_worker(frame, instance, work);
    if (work.contains("transfer") || (frame.communication && frame.communication->operation == "transfer")) {
        auto & communication = frame.communication;
        if (!communication || communication->operation != "transfer") throw std::invalid_argument("native transfer requires a transfer template");
        const auto & transfer = work.at("transfer");

        // Bind the native route before instantiation validates endpoint/resource ownership.
        communication->channel = transfer.at("channel").get<std::string>();
        communication->members = transfer.at("members").get<std::vector<std::int64_t>>();
        communication->required = communication->members;
    }
}

void WorkerBindings::bind_worker(TemplateFrame & frame, const std::string & instance, const nlohmann::json & work) const {
    if (const auto site = sites_.find(frame.site); site != sites_.end()) frame.site = site->second;

    // Local and remote pools each use their own initialized layer correspondence.
    for (auto & value : frame.state.kv) value.layer = layers_.at(value.pool).at(value.layer);
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
            if (frame.bindings.contains("required_peers")) {

                // Source-declared peer slots bind to this invocation, never to the representative's old traffic.
                communication.required.clear();
                const auto & key = frame.bindings.at("required_peers").get_ref<const std::string &>();
                for (const auto peer : work.at("expert_peers").at(key).get<std::vector<std::size_t>>())
                    communication.required.push_back(members.at(peer));
            }
        }
    }
    for (auto & child : frame.children) bind_worker(child, instance, work);
}

} // namespace markov::sglang
