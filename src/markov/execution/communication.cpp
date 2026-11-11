#include "markov/execution/communication.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <tuple>

namespace markov {

void CommunicationLinks::insert(const CoarseGraph & graph, std::span<const std::size_t> events, Timeline & timeline) {
    std::set<std::pair<std::string, std::size_t>> orders;
    std::vector<const GraphScope *> scopes;
    for (const auto & scope : graph.scopes) {
        if (scope.communication) {
            const auto & group = *scope.communication;
            if (!orders.emplace(group.channel, group.order).second) throw std::invalid_argument("action repeats a communication channel position");
            scopes.push_back(&scope);
        }
    }
    std::ranges::sort(scopes, [](const auto * left, const auto * right) {
        const auto & first = *left->communication;
        const auto & second = *right->communication;
        return std::tie(first.channel, first.order) < std::tie(second.channel, second.order);
    });
    for (const auto * scope : scopes) {
        const auto & group = *scope->communication;
        const auto & port = *std::ranges::find_if(scope->ports, [](const auto & value) { return value.rendezvous.has_value(); });
        std::map<std::tuple<Direction, std::string, bool>, TensorLayout> payload;
        if (group.operation == "transfer") {
            if (group.members.size() != 2 || group.required != group.members) throw std::invalid_argument("transfer requires one sender and one receiver");
            const auto direction = group.rank == group.members.front() ? Direction::Input : Direction::Output;
            for (const auto & field : scope->ports) {
                if (field.use != PortUse::Data && field.use != PortUse::Metadata) continue;
                if (!payload.empty() || field.direction != direction) throw std::invalid_argument("transfer requires one payload with the endpoint's direction");

                // State-backed transfers expose payload geometry; KV accesses own their contents and hazards.
                if (!field.tensor) throw std::invalid_argument("transfer payload has no layout");
                payload.emplace(std::tuple{ Direction::Input, std::string("payload"), true }, TensorLayout{ field.tensor->dtype, field.tensor->shape });
            }
            if (payload.empty()) throw std::invalid_argument("transfer has no payload");
        }
        for (const auto & agreement : group.agreements) {
            const auto & tensor = *scope->port(agreement.port, agreement.direction).tensor;

            // Variable-size protocols can agree on dtype without requiring equal per-rank row counts.
            payload.emplace(std::tuple{ agreement.direction, agreement.port, agreement.shape },
                            TensorLayout{ tensor.dtype, agreement.shape ? tensor.shape : std::vector<std::int64_t>{} });
        }

        // The channel's native protocol orders calls; source observation or batch indices never identify target work.
        const auto key = std::pair{ group.channel, positions_[{ group.channel, group.rank }]++ };
        auto [position, inserted] = pending_.try_emplace(key, Exchange{ group.operation, group.members, payload, {} });
        auto & exchange = position->second;
        if (!inserted && (exchange.operation != group.operation || exchange.members != group.members || exchange.payload != payload))
            throw std::invalid_argument("communication participants disagree on channel " + group.channel);
        if (payload.empty()) ++unknown_endpoints_;
        else ++payload_endpoints_;
        for (const auto & [field, _] : payload) {
            if (std::get<2>(field)) ++shape_fields_;
            else ++dtype_fields_;
        }
        exchange.endpoints.emplace(group.rank, Endpoint{ events[*port.rendezvous], events[port.event], group.required });
        for (auto & [rank, endpoint] : exchange.endpoints) {
            if (endpoint.connected || !std::ranges::all_of(endpoint.required, [&](const auto peer) { return exchange.endpoints.contains(peer); })) continue;
            std::vector<std::size_t> sources;
            for (const auto peer : endpoint.required) sources.push_back(exchange.endpoints.at(peer).ready);
            timeline.resolve(endpoint.completion, sources);
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
        for (const auto & [rank, endpoint] : exchange.endpoints) retained.insert(endpoint.ready);
    }
    return retained;
}

} // namespace markov
