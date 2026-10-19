#include "markov/execution/execution_graph.hpp"

#include "markov/graph/cpu_service_partition.hpp"

#include <algorithm>
#include <utility>

namespace markov {

void ExecutionGraph::group_cpu_services(std::size_t first_node, std::span<const InvokeStep> invocations, std::span<const BoundaryId> interfaces) {
    CpuServicePartition partition(boundaries_.size());
    for (std::size_t point = 0; point < boundaries_.size(); ++point) partition.name(point, boundaries_[point].cpu);
    for (const auto & edge : edges_) partition.connect(edge.source, edge.target, edge.relation);

    // Only temporal fragments between observable cuts may share a cost.
    // Waits, transfers, dependencies and controller ports retain their timing.
    const auto limit = nodes_.size();
    for (auto index = first_node; index < limit; ++index) {
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

    for (auto & group : partition.units()) {
        const auto index = group.members.front();
        auto unit = nodes_[index];
        unit.end = group.end;
        unit.cpu_members.assign(group.members.begin() + 1, group.members.end());

        const auto invocation = std::ranges::find(invocations, unit.work, &InvokeStep::work_id);
        unit.cpu = CpuServiceWork::from_input(std::move(group.service), invocation->step, unit.rank, invocation->input);

        if (unit.cpu_members.empty() && unit.end == nodes_[index].end) nodes_[index].cpu = std::move(unit.cpu);
        else {
            nodes_[index].role = TemplateNodeRole::Annotation;
            for (const auto member : unit.cpu_members) nodes_[member].role = TemplateNodeRole::Annotation;
            nodes_.push_back(std::move(unit));
        }
    }
}

} // namespace markov
