#include "markov/controller/native_transfer.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <utility>

namespace markov {

NativeTransfer::NativeTransfer(std::span<const NativeTransferRoute> routes) : routes_(routes.begin(), routes.end()) {}

nlohmann::json NativeTransfer::call(const nlohmann::json & message) {
    const auto request = message.at("request").get<std::size_t>();
    const auto group = message.at("group").get<std::string>();
    auto & handoff = handoffs_[request];
    auto & endpoint = handoff.endpoints[group];
    const auto kind = message.at("kind").get<std::string>();
    if (kind == "transfer_sender" || kind == "transfer_receiver") endpoint.registered = true;
    else if (kind != "transfer_poll") throw std::invalid_argument("unknown native transfer call: " + kind);

    bool ready = true;
    bool completed = true;
    nlohmann::json payload;
    for (const auto & route : routes_) {
        const auto & binding = route.binding;
        if (group != binding.source && group != binding.destination) continue;
        const auto & source = handoff.endpoints[binding.source];
        const auto & destination = handoff.endpoints[binding.destination];
        ready &= group == binding.source ? destination.ready : source.registered && destination.registered;
        completed &= handoff.completed.contains(binding);
        if (group == binding.destination && handoff.completed.contains(binding)) {

            // Source stages carry the same request metadata after their output-ring result.
            if (!payload.is_null() && payload != source.payload) throw std::invalid_argument("native transfer fragments disagree on metadata");
            payload = source.payload;
        }
    }
    return {
        {   "state",
         completed ? "completed"
         : ready   ? "ready"
         : "bootstrapping"                                            },
        { "payload", completed ? std::move(payload) : nlohmann::json{} }
    };
}

void NativeTransfer::submit(const ServingAction & action, const nlohmann::json & payload) {
    const auto & member = action.batch_work().batch.members.front();
    auto & endpoint = handoffs_.at(member.request).endpoints.at(action.task.group);
    if (!endpoint.payload.is_null() || member.context_tokens != endpoint.submitted_tokens || action.batch_work().final_transfer != !payload.is_null())
        throw std::invalid_argument("native KV fragments must be contiguous with metadata only on the last send");

    endpoint.submitted_tokens += member.new_tokens;
    endpoint.payload = payload;
    endpoint.fragments.emplace(action.task.sequence, Fragment{ member, action.batch_work().final_transfer, false, {}, {} });
}

std::optional<ServingAction> NativeTransfer::plan(std::int64_t now_ns) {
    for (auto & [request, handoff] : handoffs_) {
        for (const auto & route : routes_) {
            const auto & binding = route.binding;
            auto & source = handoff.endpoints[binding.source];
            if (!handoff.endpoints[binding.destination].ready) continue;
            for (auto & [flow, fragment] : source.fragments) {
                if (!fragment.ready || fragment.submitted.contains(binding)) continue;
                const std::array demands{
                    KvDemand{ request, 0 }
                };
                auto batch = route.destination.plan(Phase::Prefill, demands);
                auto & member = batch.members.front();
                member.context_tokens = fragment.member.context_tokens;
                member.new_tokens = fragment.member.new_tokens;
                BatchWork work{ std::move(batch), {}, {} };
                work.final_transfer = fragment.final;
                const auto & source_pages = route.source.residence(request).pages;
                const auto & destination_pages = route.destination.residence(request).pages;
                const auto begin = route.source.pages_for(fragment.member.context_tokens);
                const auto end = route.source.pages_for(fragment.member.context_tokens + fragment.member.new_tokens);
                for (auto page = begin; page < end; ++page) work.transferred_pages.push_back({ source_pages.at(page), destination_pages.at(page) });
                fragment.submitted.insert(binding);

                // Copy flow names the source send task; each copy keeps its own transfer task identity.
                return ServingAction{
                    { "kv", sequence_++ },
                    std::make_shared<const BatchWork>(std::move(work)), now_ns, flow, 0, std::nullopt, ServingStep::Transfer, {},
                    binding
                };
            }
        }
    }
    return std::nullopt;
}

void NativeTransfer::finish(const ServingAction & action) {
    const bool sent = action.step == ServingStep::Send && action.channel.empty();
    const bool received = action.step == ServingStep::Receive && action.channel.empty();
    const bool released =
        action.step == ServingStep::Release && std::ranges::any_of(routes_, [&](const auto & route) { return route.binding.source == action.task.group; });
    if (action.step != ServingStep::Reserve && action.step != ServingStep::Transfer && !sent && !received && !released) return;

    for (const auto & member : action.batch_work().batch.members) {
        auto & handoff = handoffs_.at(member.request);
        if (action.step == ServingStep::Transfer) {
            const auto & binding = *action.transfer;
            auto & fragments = handoff.endpoints.at(binding.source).fragments;
            auto & fragment = fragments.at(action.flow);
            fragment.completed.insert(binding);
            if (fragment.final) handoff.completed.insert(binding);
            const auto destinations = std::ranges::count_if(routes_, [&](const auto & route) { return route.binding.source == binding.source; });
            if (fragment.completed.size() == static_cast<std::size_t>(destinations)) fragments.erase(action.flow);
        }
        else {
            auto & endpoint = handoff.endpoints.at(action.task.group);
            if (sent) endpoint.fragments.at(action.task.sequence).ready = true;
            if (action.step == ServingStep::Reserve) endpoint.ready = true;
            if (received || released) endpoint.consumed = true;
        }

        if (handoff.completed.size() == routes_.size() && std::ranges::all_of(handoff.endpoints, [](const auto & entry) { return entry.second.consumed; }))
            handoffs_.erase(member.request);
    }
}

bool NativeTransfer::running() const {
    for (const auto & [_, handoff] : handoffs_)
        for (const auto & [_, endpoint] : handoff.endpoints)
            for (const auto & [_, fragment] : endpoint.fragments)
                if (fragment.submitted.size() != fragment.completed.size()) return true;
    return false;
}

bool NativeTransfer::settled() const { return handoffs_.empty(); }

} // namespace markov
