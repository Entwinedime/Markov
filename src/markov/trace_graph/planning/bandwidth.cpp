#include "markov/trace_graph/planning/bandwidth.hpp"

#include <utility>

namespace markov::trace_graph::planning {

Bandwidth::Bandwidth(BandwidthLimits limits, std::size_t participants, std::size_t resource_begin)
    : limits_(limits),
      participants_(participants),
      resource_begin_(resource_begin),
      tails_(2 * participants + 3) {}

graph::NodeId Bandwidth::admit(PlannedMutation & mutation, graph::NodeId begin, TransferDirection direction, std::size_t bytes, std::size_t rank,
                               std::optional<graph::NodeId> cause) {
    const auto link = std::to_underlying(direction);
    const auto lane = link < 2 ? 2 * rank + link : 2 * participants_ + link - 2;
    const auto gate = begin + mutation.operations.size();
    mutation.operations.push_back({ 0, Boundary{} });
    if (cause) mutation.dependencies.push_back({ *cause, gate, graph::EdgeKind::DataDependency });

    for (std::size_t index = 0; index < (link >= 2 ? 2u : 1u); ++index) {
        const auto channel = index ? 2 * participants_ + 2 : lane;
        const auto rate = index ? limits_.storage_shared_bytes_per_sec : limits_.bytes_per_sec[link];
        if (tails_[channel]) mutation.dependencies.push_back({ *tails_[channel], gate, graph::EdgeKind::ResourceOrder });
        const auto node = begin + mutation.operations.size();
        PlannedOperation operation{ resource_begin_ + channel, Boundary{} };
        operation.reservation = BandwidthReservation{ bytes, rate };
        operation.position = OperationPosition{ std::nullopt, 0, rank, "bandwidth_reservation" };
        mutation.operations.push_back(std::move(operation));
        mutation.dependencies.push_back({ gate, node, graph::EdgeKind::DataDependency });
        tails_[channel] = node;
    }
    return gate;
}

} // namespace markov::trace_graph::planning
