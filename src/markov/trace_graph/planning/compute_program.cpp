#include "markov/trace_graph/planning/compute_program.hpp"

#include <functional>
#include <map>
#include <utility>
#include <vector>

namespace markov::trace_graph::planning {

PlannedMutation compute_mutation(const source::SourceContext & source, const ComputePlan & plan, source::Index frame, graph::NodeId begin,
                                 simulation::Component owner, std::optional<simulation::Action> completed) {
    struct Ports {
        graph::NodeId first, last;
    };
    PlannedMutation mutation{ owner, {}, {} };
    std::map<source::Index, Ports> ports;
    for (const auto & operation : plan.operations) {
        const auto resource = operation.work ? source.device_service(operation.source_operation).resource.value() : 0;
        const auto id = begin + mutation.operations.size();
        const auto [port, inserted] = ports.try_emplace(operation.source_operation, Ports{ id, id });
        port->second.last = id;
        if (operation.work) mutation.operations.push_back({ resource, std::cref(plan.work[*operation.work]) });
        else mutation.operations.push_back({ resource, Boundary{} });
    }

    std::vector<bool> has_successor(plan.operations.size(), false);
    for (const auto & edge : plan.dependencies) {
        mutation.dependencies.push_back({ begin + edge.from, begin + edge.to, edge.kind });
        has_successor[edge.from] = true;
    }
    for (const auto & order : source.frame_queue_order(frame)) {
        const auto before = ports.at(order.before).last, after = ports.at(order.after).first;
        mutation.dependencies.push_back({ before, after, graph::EdgeKind::ResourceOrder });
        has_successor[before - begin] = true;
    }
    const auto completion = begin + mutation.operations.size();
    mutation.operations.push_back({ 0, Boundary{}, completed });
    for (std::size_t id = 0; id < has_successor.size(); ++id) {
        if (!has_successor[id]) mutation.dependencies.push_back({ begin + id, completion, graph::EdgeKind::DataDependency });
    }
    return mutation;
}

void ComputeProgram::start(ProgramRuntime & runtime) {
    runtime.submit(compute_mutation(source_, plan_, frame_, runtime.next_node(), 0, simulation::Action{ 0, 0 }));
}

void ComputeProgram::on_event(ProgramRuntime &, const simulation::Event &) { complete_ = true; }

} // namespace markov::trace_graph::planning
