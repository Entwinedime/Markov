#include "markov/templates/communication_work.hpp"

#include <algorithm>

namespace markov {

std::expected<std::vector<SourceCommunicationTask>, std::string> CommunicationWorkLayout::evaluate(std::span<const SourceCommunicationTask> source,
                                                                                                   std::uint64_t count) const {
    for (const auto & domain : domains_) {
        if (count < domain.minimum || count > domain.maximum) return std::unexpected(domain.gap);
    }

    // The producer bounds count so payload and aligned partition arithmetic
    // are representable. The final partition keeps only the remaining bytes.
    const auto payload = count * element_bytes_;
    const auto share = payload / partitions_ + (payload % partitions_ != 0);
    const auto width = (share / alignment_ + (share % alignment_ != 0)) * alignment_;
    std::vector<SourceCommunicationTask> tasks(source.begin(), source.end());
    for (const auto & relation : relations_) {
        auto bytes = payload;
        if (relation.extent == CommunicationByteExtent::DestinationPartition) {
            // The qualified producer validates destination ranks against the
            // partition domain; rank is task data, not a rule constant.
            const auto offset = std::min(payload, width * tasks[relation.task].destination_rank);
            bytes = std::min(width, payload - offset);
        }
        tasks[relation.task].bytes = bytes;
    }
    return tasks;
}

std::uint64_t CollectiveCountRule::count(const BatchInput & input) const {
    std::uint64_t extent = input.requests.size();
    if (rows == CollectiveRows::Tokens) {
        extent = 0;
        for (const auto & request : input.requests) extent += request.input_tokens;
    }
    return extent * width;
}

std::strong_ordering CollectiveParameterRule::operator<=>(const CollectiveParameterRule & other) const {
    if (const auto order = count <=> other.count; order != 0) return order;
    if (const auto order = work.has_value() <=> other.work.has_value(); order != 0) return order;
    return work ? *work <=> *other.work : work.error() <=> other.work.error();
}

} // namespace markov
