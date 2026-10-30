#include "markov/trace_graph/planning/serial_program.hpp"

#include <functional>
#include <utility>

namespace markov::trace_graph::planning {

void SerialProgram::start(ProgramRuntime & runtime) {
    PlannedMutation mutation{ 0, {}, {} };
    const auto first = runtime.next_node();
    for (const auto & request : requests_) {
        const auto start = first + mutation.operations.size();
        mutation.operations.push_back({ 0, Boundary{} });
        mutation.operations.push_back({
            0,
            SourceClientDelay{ request.source_request, false }
        });
        mutation.operations.push_back({ 0, SourceRegionGap{ request.before_execution } });
        if (runtime.records_graph()) mutation.operations.back().position = OperationPosition{ request.source_request, 0, 0, "before_execution" };
        for (std::size_t at = 0; at < request.work.size(); ++at) {
            mutation.operations.push_back({ 0, std::cref(request.work[at]) });
            if (runtime.records_graph())
                mutation.operations.back().position = OperationPosition{ request.source_request, at ? at - 1 : 0, 0, at ? "phase" : "control" };
        }
        mutation.operations.push_back({ 0, SourceRegionGap{ request.after_execution } });
        if (runtime.records_graph()) mutation.operations.back().position = OperationPosition{ request.source_request, 0, 0, "after_execution" };
        mutation.operations.push_back({
            0,
            SourceClientDelay{ request.source_request, true }
        });
        const auto response = first + mutation.operations.size() - 1;
        if (!ports_.empty()) mutation.dependencies.push_back({ ports_.back().response, start, graph::EdgeKind::ClientOrder });
        for (auto node = start + 1; node <= response; ++node) mutation.dependencies.push_back({ node - 1, node, graph::EdgeKind::DataDependency });
        ports_.push_back({ start, response });
    }
    mutation.operations.back().completion = simulation::Action{ 0, 0 };
    runtime.submit(std::move(mutation));
}

} // namespace markov::trace_graph::planning
