#include "markov/execution/communication.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <tuple>

namespace markov {

void CommunicationLinks::insert(const CoarseGraph & graph, std::span<const std::size_t> events,
                                std::span<const ServiceDuration> durations, Timeline & timeline) {
    std::set<std::pair<std::string, std::size_t>> orders;
    std::vector<std::size_t> scopes;
    for (std::size_t scope = 0; scope < graph.scopes.size(); ++scope) {
        if (const auto & communication = graph.scopes[scope].communication) {
            const auto & group = *communication;
            if (!orders.emplace(group.channel, group.order).second) throw std::invalid_argument("action repeats a communication channel position");
            scopes.push_back(scope);
        }
    }
    if (scopes.empty()) return;

    std::vector<std::optional<Collective>> collectives(graph.scopes.size());
    for (std::size_t index = 0; index < graph.work.size(); ++index) {
        const auto & work = graph.work[index];
        if (work.kind == WorkKind::Collective)
            collectives[work.scope] = Collective{ events[work.start], events[work.finish], durations[index].value_or(0) };
    }
    std::ranges::sort(scopes, [&](auto left, auto right) {
        const auto & first = *graph.scopes[left].communication;
        const auto & second = *graph.scopes[right].communication;
        return std::tie(first.channel, first.order) < std::tie(second.channel, second.order);
    });
    for (const auto scope_id : scopes) {
        const auto & scope = graph.scopes[scope_id];
        const auto & group = *scope.communication;
        const auto & port = *std::ranges::find_if(scope.ports, [](const auto & value) { return value.rendezvous.has_value(); });
        const auto & collective = collectives[scope_id];
        std::map<std::tuple<Direction, std::string, bool>, TensorLayout> payload;
        if (group.operation == "transfer") {
            if (group.members.size() != 2 || group.required != group.members) throw std::invalid_argument("transfer requires one sender and one receiver");
            const auto direction = group.rank == group.members.front() ? Direction::Input : Direction::Output;
            for (const auto & field : scope.ports) {
                if (field.use != PortUse::Data && field.use != PortUse::Metadata) continue;
                if (!payload.empty() || field.direction != direction) throw std::invalid_argument("transfer requires one payload with the endpoint's direction");

                // State-backed transfers expose payload geometry; KV accesses own their contents and hazards.
                if (!field.tensor) throw std::invalid_argument("transfer payload has no layout");
                payload.emplace(std::tuple{ Direction::Input, std::string("payload"), true }, TensorLayout{ field.tensor->dtype, field.tensor->shape });
            }
            if (payload.empty()) throw std::invalid_argument("transfer has no payload");
        }
        for (const auto & agreement : group.agreements) {
            const auto & tensor = *scope.port(agreement.port, agreement.direction).tensor;

            // Variable-size protocols can agree on dtype without requiring equal per-rank row counts.
            payload.emplace(std::tuple{ agreement.direction, agreement.port, agreement.shape },
                            TensorLayout{ tensor.dtype, agreement.shape ? tensor.shape : std::vector<std::int64_t>{} });
        }

        // The channel's native protocol orders calls; source observation or batch indices never identify target work.
        const auto key = std::pair{ group.channel, positions_[{ group.channel, group.rank }]++ };
        auto [position, inserted] = pending_.try_emplace(key, Exchange{ group.operation, group.members, payload, collective.has_value(), {} });
        auto & exchange = position->second;
        if (!inserted && (exchange.operation != group.operation || exchange.members != group.members || exchange.payload != payload
            || exchange.collective != collective.has_value()))
            throw std::invalid_argument("communication participants disagree on channel " + group.channel);
        if (payload.empty()) ++unknown_endpoints_;
        else ++payload_endpoints_;
        for (const auto & [field, _] : payload) {
            if (std::get<2>(field)) ++shape_fields_;
            else ++dtype_fields_;
        }
        exchange.endpoints.emplace(group.rank, Endpoint{ events[*port.rendezvous], events[port.event], group.required, collective });
        for (auto & [rank, endpoint] : exchange.endpoints) {
            if (endpoint.connected || !std::ranges::all_of(endpoint.required, [&](const auto peer) { return exchange.endpoints.contains(peer); })) continue;
            std::vector<std::size_t> sources;
            for (const auto peer : endpoint.required) {
                const auto & input = exchange.endpoints.at(peer);
                sources.push_back(input.collective ? input.collective->ready : input.ready);
            }
            if (const auto & work = endpoint.collective) {
                timeline.resolve(work->finish, sources, Relation::Communication, {}, work->service_ns);

                // Call visibility follows transport, while caller-stream copies keep their own dependencies.
                sources.clear();
                if (work->finish != endpoint.completion) sources.push_back(work->finish);
            }
            timeline.resolve(endpoint.completion, sources, endpoint.collective ? Relation::Order : Relation::Communication);
            endpoint.connected = true;
        }

        // Broadcast's source can return before receivers arrive; retain its input boundary for those later calls.
        if (exchange.endpoints.size() == exchange.members.size()) pending_.erase(position);
    }
}

bool CommunicationLinks::empty() const { return pending_.empty(); }

nlohmann::json CommunicationLinks::payload_coverage() const {
    return { { "checked_endpoints", payload_endpoints_ }, { "undeclared_endpoints", unknown_endpoints_ },
             { "shape_fields", shape_fields_ }, { "dtype_only_fields", dtype_fields_ } };
}

std::set<std::size_t> CommunicationLinks::retained_events() const {
    std::set<std::size_t> retained;
    for (const auto & [_, exchange] : pending_) {
        for (const auto & [rank, endpoint] : exchange.endpoints)
            retained.insert(endpoint.collective ? endpoint.collective->ready : endpoint.ready);
    }
    return retained;
}

} // namespace markov
