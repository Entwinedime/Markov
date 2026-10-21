#include "markov/execution/execution_graph.hpp"

#include "markov/graph/cpu_service_partition.hpp"

#include <algorithm>
#include <utility>

namespace markov {

void ExecutionGraph::group_cpu_services(std::span<TemplateInstance *> instances, std::span<const InvokeStep> invocations,
                                        std::span<const BoundaryId> interfaces) {
    std::map<std::pair<WorkId, std::uint32_t>, CpuPublicationWork> publications;
    for (const auto * instance : instances) {
        if (!instance->definition->publication_layout) continue;
        const auto invocation = std::ranges::find(invocations, instance->work_id, &InvokeStep::work_id);
        auto work = instance->definition->publication_layout->cpu_work(*invocation->input);
        if (work) publications.emplace(std::pair{ instance->work_id, instance->rank }, std::move(*work));
    }

    CpuServicePartition partition(boundaries_.size());
    for (std::size_t point = 0; point < boundaries_.size(); ++point) partition.name(point, boundaries_[point].cpu);
    for (const auto & edge : edges_) partition.connect(edge.source, edge.target, edge.relation);

    // Only temporal fragments between observable cuts may share a cost.
    // Waits, transfers, dependencies and controller ports retain their timing.
    const auto limit = nodes_.size();
    for (std::size_t index = 0; index < limit; ++index) {
        const auto & node = nodes_[index];
        if (node.kind == SourceSpanKind::CpuControl && !node.transfer) partition.add({ index, node.begin, node.end, node.work, node.rank });
        else if (node.kind == SourceSpanKind::HostWait) partition.wait(node.begin, node.end);
        else if (node.role != TemplateNodeRole::Annotation) {
            partition.protect(node.begin);
            partition.protect(node.end);
        }
    }
    for (const auto point : interfaces) partition.protect(point);
    for (const auto & port : ports_) partition.protect(port.boundary);

    auto groups = partition.units();
    bool split = false;
    for (const auto & group : groups)
        for (std::size_t member = 1; member < group.members.size(); ++member) {
            const auto & before = nodes_[group.members[member - 1]];
            const auto & after = nodes_[group.members[member]];
            if (before.collective == after.collective && before.input_slot == after.input_slot) continue;
            partition.protect(before.end);
            partition.protect(after.begin);
            split = true;
        }
    if (split) groups = partition.units();
    for (auto & group : groups) {
        const auto index = group.members.front();
        auto unit = nodes_[index];
        unit.end = group.end;

        const auto invocation = std::ranges::find(invocations, unit.work, &InvokeStep::work_id);
        const auto publication = publications.find({ unit.work, unit.rank });
        unit.cpu = CpuServiceWork::from_input(std::move(group.service),
                                              invocation->step,
                                              unit.rank,
                                              *invocation->input,
                                              publication == publications.end() ? std::nullopt : std::optional(publication->second));
        if (unit.collective) unit.cpu->collective = *unit.collective;
        if (unit.input_slot) {
            if (!unit.cpu->scheduling || *unit.input_slot >= invocation->input->incoming_requests.size()) {
                if (!assembly_gap_) assembly_gap_ = ExecutionTimingGap{ "cpu_input_handler_work_not_bound", unit.work, unit.rank, unit.source };
            }
            else {
                const auto & input = invocation->input->incoming_requests[*unit.input_slot];
                unit.cpu->scheduling->incoming_requests = {
                    { input.prompt_tokens, input.output_tokens }
                };
            }
        }

        if (group.members.size() == 1 && unit.end == nodes_[index].end) nodes_[index].cpu = std::move(unit.cpu);
        else {
            for (const auto member : group.members) nodes_[member].role = TemplateNodeRole::Annotation;
            nodes_.push_back(std::move(unit));

            // A timing unit can cross semantic scopes. Keep every member,
            // including the first, instead of assigning its scope to the group.
            if (diagnostics_.is_open())
                logical_scopes_.push_back({
                    {      "binding", "cpu_aggregate" },
                    { "member_nodes",   group.members }
                });
        }
    }
}

} // namespace markov
