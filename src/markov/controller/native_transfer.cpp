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
    else if (kind == "transfer_submit") {
        if (!endpoint.payload.is_null()) throw std::invalid_argument("native transfer submitted twice");
        endpoint.payload = message.at("payload");
    }
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

std::optional<ServingAction> NativeTransfer::plan(std::int64_t now_ns) {
    for (auto & [request, handoff] : handoffs_) {
        for (const auto & route : routes_) {
            const auto & binding = route.binding;
            const auto & source = handoff.endpoints[binding.source];
            const auto & destination = handoff.endpoints[binding.destination];
            if (!source.ready || !destination.ready || source.payload.is_null() || handoff.submitted.contains(binding)) continue;
            const std::array demands{
                KvDemand{ request, 0 }
            };
            auto batch = route.destination.plan(Phase::Prefill, demands);
            BatchWork work{ std::move(batch), {}, {} };
            const auto & source_pages = route.source.residence(request).pages;
            const auto & destination_pages = route.destination.residence(request).pages;
            for (std::size_t page = 0; page < source_pages.size(); ++page) work.transferred_pages.push_back({ source_pages[page], destination_pages.at(page) });
            handoff.submitted.insert(binding);
            const auto sequence = sequence_++;
            return ServingAction{
                { "kv", sequence },
                std::make_shared<const BatchWork>(std::move(work)), now_ns, sequence, 0, std::nullopt, ServingStep::Transfer, {},
                binding
            };
        }
    }
    return std::nullopt;
}

void NativeTransfer::finish(const ServingAction & action) {
    const bool source = std::ranges::any_of(routes_, [&](const auto & route) { return route.binding.source == action.task.group; });
    const bool received = action.step == ServingStep::Receive && action.channel.empty();
    if (action.step != ServingStep::Reserve && action.step != ServingStep::Transfer && !(action.step == ServingStep::Result && source)
        && !(action.step == ServingStep::Release && source) && !received)
        return;

    for (const auto & member : action.work->batch.members) {
        auto & handoff = handoffs_.at(member.request);
        if (action.step == ServingStep::Transfer) handoff.completed.insert(action.transfer.value());
        else {
            auto & endpoint = handoff.endpoints.at(action.task.group);
            if (action.step == ServingStep::Reserve || action.step == ServingStep::Result) endpoint.ready = true;
            if (received || action.step == ServingStep::Release) endpoint.consumed = true;
        }

        if (handoff.completed.size() == routes_.size() && std::ranges::all_of(handoff.endpoints, [](const auto & entry) { return entry.second.consumed; }))
            handoffs_.erase(member.request);
    }
}

bool NativeTransfer::running() const {
    return std::ranges::any_of(handoffs_, [](const auto & entry) { return entry.second.submitted.size() != entry.second.completed.size(); });
}

bool NativeTransfer::settled() const { return handoffs_.empty(); }

} // namespace markov
