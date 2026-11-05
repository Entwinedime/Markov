#include "markov/controller/native_transfer.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <utility>

namespace markov {

NativeTransfer::NativeTransfer(std::span<const NativeTransferRoute> routes) : routes_(routes.begin(), routes.end()) {}

nlohmann::json NativeTransfer::poll(const std::string & group, std::size_t request) const {
    const auto found = handoffs_.find(request);
    if (found == handoffs_.end())
        return {
            {   "state", "bootstrapping" },
            { "payload",         nullptr }
        };
    const auto & handoff = found->second;
    if (const auto source = handoff.sources.find(group); source != handoff.sources.end()) {
        const auto & endpoint = source->second;
        return {
            {   "state",
             endpoint.published == Progress::Complete   ? "completed"
             : endpoint.connected == Progress::Complete ? "ready"
             : "bootstrapping"                                            },
            { "payload",                                           nullptr }
        };
    }
    const auto destination = handoff.destinations.find(group);
    if (destination == handoff.destinations.end())
        return {
            {   "state", "bootstrapping" },
            { "payload",         nullptr }
        };
    if (destination->second.published != Progress::Complete)
        return {
            {   "state", "ready" },
            { "payload", nullptr }
        };

    nlohmann::json payload;
    for (const auto & route : routes_) {
        if (route.binding.destination != group) continue;
        const auto & source = handoff.sources.at(route.binding.source);

        // Source stages carry the same request metadata after their output-ring result.
        if (!payload.is_null() && payload != source.payload) throw std::invalid_argument("native transfer fragments disagree on metadata");
        payload = source.payload;
    }
    return {
        {   "state",        "completed" },
        { "payload", std::move(payload) }
    };
}

void NativeTransfer::submit(const ServingAction & action, const nlohmann::json & payload) {
    const auto & member = action.batch_work().batch.members.front();
    auto & endpoint = handoffs_.at(member.request).sources.at(action.task.group);
    if (!endpoint.payload.is_null() || member.context_tokens != endpoint.submitted_tokens || action.batch_work().final_transfer != !payload.is_null())
        throw std::invalid_argument("native KV fragments must be contiguous with metadata only on the last send");

    endpoint.submitted_tokens += member.new_tokens;
    endpoint.payload = payload;
    endpoint.fragments.emplace(action.task.sequence, Fragment{ member, action.batch_work().final_transfer, false, {}, {} });
}

ServingAction NativeTransfer::endpoint_event(std::size_t request, const std::string & group, ServingStep step, std::int64_t now_ns) {
    return {
        {    "kv", sequence_++ },
        TransferEndpoint{ request,       group },
        now_ns, 0, 0, std::nullopt, step
    };
}

std::optional<ServingAction> NativeTransfer::plan(std::int64_t now_ns) {
    for (auto & [request, handoff] : handoffs_) {
        for (auto & [group, source] : handoff.sources) {
            if (source.connected == Progress::Waiting && std::ranges::all_of(routes_, [&](const auto & route) {
                    return route.binding.source != group || handoff.destinations.contains(route.binding.destination);
                })) {
                source.connected = Progress::Submitted;
                return endpoint_event(request, group, ServingStep::Handshake, now_ns);
            }

            // Native sender success follows all its destinations, rather than each rank-pair copy.
            if (source.published == Progress::Waiting && !source.payload.is_null()
                && std::ranges::all_of(routes_, [&](const auto & route) { return route.binding.source != group || handoff.copied.contains(route.binding); })) {
                source.published = Progress::Submitted;
                return endpoint_event(request, group, ServingStep::Notify, now_ns);
            }
        }

        for (const auto & route : routes_) {
            const auto & binding = route.binding;
            const auto source_entry = handoff.sources.find(binding.source);
            if (source_entry == handoff.sources.end() || source_entry->second.connected != Progress::Complete) continue;
            auto & source = source_entry->second;
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

                // Copy flow names the source send task; the detached copy keeps its own task identity.
                return ServingAction{
                    { "kv", sequence_++ },
                    std::make_shared<const BatchWork>(std::move(work)), now_ns, flow, 0, std::nullopt, ServingStep::Transfer, {},
                    binding
                };
            }
        }

        for (auto & [group, destination] : handoff.destinations) {
            if (destination.published == Progress::Waiting && std::ranges::all_of(routes_, [&](const auto & route) {
                    return route.binding.destination != group || handoff.sources.at(route.binding.source).published == Progress::Complete;
                })) {
                destination.published = Progress::Submitted;
                return endpoint_event(request, group, ServingStep::Notify, now_ns);
            }
        }
    }
    return std::nullopt;
}

void NativeTransfer::start(const ServingAction & action) {
    if (action.step != ServingStep::Advertise) return;
    const auto request = action.batch_work().batch.members.front().request;
    auto & handoff = handoffs_[request];

    // Advertising creates only transport intent; source registration does not gate native bootstrap reception.
    if (!handoff.destinations.try_emplace(action.task.group).second) throw std::invalid_argument("native destination advertised the same live handoff twice");
    for (const auto & route : routes_) {
        if (route.binding.destination == action.task.group) handoff.sources.try_emplace(route.binding.source);
    }
}

void NativeTransfer::finish(const ServingAction & action) {
    if (const auto * event = std::get_if<TransferEndpoint>(&action.work)) {
        auto & handoff = handoffs_.at(event->request);
        if (action.step == ServingStep::Handshake) handoff.sources.at(event->group).connected = Progress::Complete;
        else if (const auto source = handoff.sources.find(event->group); source != handoff.sources.end()) source->second.published = Progress::Complete;
        else handoff.destinations.at(event->group).published = Progress::Complete;
        return;
    }

    const bool sent = action.step == ServingStep::Send && action.channel.empty();
    const bool received = action.step == ServingStep::Receive && action.channel.empty();
    const bool released =
        action.step == ServingStep::Release && std::ranges::any_of(routes_, [&](const auto & route) { return route.binding.source == action.task.group; });
    if (action.step != ServingStep::Transfer && !sent && !received && !released) return;

    for (const auto & member : action.batch_work().batch.members) {
        auto & handoff = handoffs_.at(member.request);
        if (action.step == ServingStep::Transfer) {
            const auto & binding = *action.transfer;
            auto & fragments = handoff.sources.at(binding.source).fragments;
            auto & fragment = fragments.at(action.flow);
            fragment.completed.insert(binding);
            if (fragment.final) handoff.copied.insert(binding);
            const auto destinations = std::ranges::count_if(routes_, [&](const auto & route) { return route.binding.source == binding.source; });
            if (fragment.completed.size() == static_cast<std::size_t>(destinations)) fragments.erase(action.flow);
        }
        else {
            if (sent) handoff.sources.at(action.task.group).fragments.at(action.task.sequence).ready = true;
            if (received) handoff.destinations.at(action.task.group).consumed = true;
            if (released) handoff.sources.at(action.task.group).released = true;
        }

        if (handoff.copied.size() == routes_.size() && std::ranges::all_of(handoff.sources, [](const auto & entry) { return entry.second.released; })
            && std::ranges::all_of(handoff.destinations, [](const auto & entry) { return entry.second.consumed; }))
            handoffs_.erase(member.request);
    }
}

bool NativeTransfer::running() const {
    for (const auto & [_, handoff] : handoffs_) {
        for (const auto & [_, source] : handoff.sources) {
            if (source.connected == Progress::Submitted || source.published == Progress::Submitted) return true;
            for (const auto & [_, fragment] : source.fragments)
                if (fragment.submitted.size() != fragment.completed.size()) return true;
        }
        if (std::ranges::any_of(handoff.destinations, [](const auto & entry) { return entry.second.published == Progress::Submitted; })) return true;
    }
    return false;
}

bool NativeTransfer::settled() const { return handoffs_.empty(); }

} // namespace markov
