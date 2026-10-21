#include "markov/templates/templates.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace markov {

std::expected<void, TemplateGap> TemplateBank::parameterize_communications(std::span<TemplateInstance> instances, const InvokeStep & invocation) const {
    using LocalMember = std::pair<TemplateInstance *, std::size_t>;
    std::map<std::pair<std::string, std::optional<std::uint32_t>>, std::vector<LocalMember>> interfaces;
    for (auto & instance : instances) {
        const auto & ports = instance.topology.collective_interfaces();
        for (std::size_t index = 0; index < ports.size(); ++index) {
            const auto & port = ports[index];
            const auto & source = port.definition->collective_interfaces[port.interface];
            interfaces[{ source.position, port.model_layer }].emplace_back(&instance, index);
        }
    }
    // Every declared TP operation includes this cohort's full rank group.
    // Reserve its owner once: local interfaces borrow these stable work values.
    instances.front().collective_groups.reserve(interfaces.size());
    for (const auto & [position, members] : interfaces) {
        const auto & first = members.front().first->topology.collective_interfaces()[members.front().second];
        const auto & source = first.definition->collective_interfaces[first.interface];
        const auto count = source.count ? source.count->count(*invocation.input) : 0;
        TemplateCollectiveGroup group{
            { source.operation == CommunicationOperation::Broadcast   ? "broadcast"
              : source.operation == CommunicationOperation::AllReduce ? "all_reduce"
                                                                      : "all_gather",
             {},
             source.count ? std::vector<std::uint64_t>{ count / source.count->width, source.count->width } : std::vector<std::uint64_t>{},
             source.data_type },
            {},
            {}
        };
        if (source.host_message) {
            group.work.host_message = HostMessageWork{ source.host_message->root, std::nullopt, {} };
            for (const auto & request : invocation.input->incoming_requests)
                group.work.host_message->incoming_requests.emplace_back(request.prompt_tokens, request.output_tokens);
        }
        std::set<std::uint32_t> ranks;
        for (const auto & [instance, index] : members) {
            const auto & port = instance->topology.collective_interfaces()[index];
            const auto & peer = port.definition->collective_interfaces[port.interface];
            if (members.size() != instances.size() || members.size() != source.group_size || !ranks.insert(instance->rank).second
                || peer.operation != source.operation || peer.group_size != source.group_size || peer.data_type != source.data_type
                || peer.count != source.count || peer.host_message != source.host_message)
                return std::unexpected(
                    TemplateGap{ TemplateGapKind::Rule, "collective_participant_work_not_paired", invocation.step, invocation.input->phase, instance->rank });
            group.work.ranks.push_back(instance->rank);
            group.ready.push_back({ instance->work_id, instance->rank, port.ready });
            group.done.push_back({ instance->work_id, instance->rank, port.done });
        }
        if (source.host_message)
            for (const auto & [entry_rank, done_rank] : source.host_message->completion_entry_dependencies) {
                const auto entry = std::ranges::find(group.ready, entry_rank, &TemplateEndpoint::rank);
                const auto done = std::ranges::find(group.done, done_rank, &TemplateEndpoint::rank);
                group.completion_entry_dependencies.emplace_back(*entry, *done);
            }
        auto & owned = instances.front().collective_groups.emplace_back(std::move(group));
        for (const auto & [instance, index] : members) instance->topology.bind_collective_work(index, owned.work);
    }

    return {};
}

} // namespace markov
