#include "markov/trace_graph/planning/compute_program.hpp"

#include <functional>
#include <map>
#include <utility>
#include <vector>

namespace markov::trace_graph::planning {

void ComputeProgram::start(ProgramRuntime & runtime) {
    struct Ports {
        graph::NodeId first, last;
    };
    const auto begin = runtime.next_node();
    PlannedMutation mutation{ 0, {}, {} };
    std::map<source::Index, Ports> ports;
    for (const auto & operation : plan_.operations) {
        const auto resource = source_.device_service(operation.source_operation).resource.value();
        const auto id = begin + mutation.operations.size();
        const auto [port, inserted] = ports.try_emplace(operation.source_operation, Ports{ id, id });
        port->second.last = id;
        mutation.operations.push_back({ resource, std::cref(plan_.work[operation.work]) });
    }

    std::vector<bool> has_successor(plan_.operations.size(), false);
    for (const auto & edge : plan_.dependencies) {
        mutation.dependencies.push_back({ begin + edge.from, begin + edge.to, edge.kind });
        has_successor[edge.from] = true;
    }
    for (const auto & order : source_.frame_queue_order(frame_)) {
        const auto before = ports.at(order.before).last, after = ports.at(order.after).first;
        mutation.dependencies.push_back({ before, after, graph::EdgeKind::ResourceOrder });
        has_successor[before - begin] = true;
    }
    const auto completion = begin + mutation.operations.size();
    mutation.operations.push_back({
        0,
        Boundary{},
        simulation::Action{ 0, 0 }
    });
    for (std::size_t id = 0; id < has_successor.size(); ++id) {
        if (!has_successor[id]) mutation.dependencies.push_back({ begin + id, completion, graph::EdgeKind::DataDependency });
    }
    runtime.submit(std::move(mutation));
}

void ComputeProgram::on_event(ProgramRuntime &, const simulation::Event &) { complete_ = true; }

} // namespace markov::trace_graph::planning
