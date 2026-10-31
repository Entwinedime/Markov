#include "markov/trace_graph/planning/storage.hpp"

#include <functional>
#include <utility>

namespace markov::trace_graph::planning {

std::optional<graph::NodeId> Storage::control_response(ProgramRuntime & runtime, std::size_t request, const WorkKind & kind, Features features,
                                                       ActionKind completion, std::string_view port, std::optional<graph::NodeId> cause) {
    features.emplace("page_tokens", static_cast<double>(page_tokens_));
    features.emplace("participants", static_cast<double>(participants_));
    const Work work{ kind, std::move(features) };
    const auto node = runtime.next_node();
    // CacheProgram holds all caller turns until this closed group returns.
    // This is a readiness response, not exclusive service on one CPU thread.
    PlannedMutation mutation{ owner, { { foreground_[request].callers.front(), std::cref(work), action(completion, request), true } }, {} };
    if (cause) mutation.dependencies.push_back({ *cause, node, graph::EdgeKind::DataDependency });
    if (runtime.records_graph()) mutation.operations.front().position = OperationPosition{ request, 0, 0, port };
    return runtime.submit(std::move(mutation));
}

std::optional<graph::NodeId> Storage::begin_prefetch(ProgramRuntime & runtime, std::size_t request, std::span<const PrefixId> lookup,
                                                     const CacheMatch & matched, std::span<const std::size_t> callers, std::optional<graph::NodeId> cause) {
    auto & result = results_[request];
    if (result.examined) return std::nullopt;
    result.examined = true;
    auto & control = foreground_[request];
    control.callers.assign(callers.begin(), callers.end());
    allocate_prefetch(request, lookup, matched);
    return control_response(runtime,
                            request,
                            backend_.issue,
                            {
                                { "candidate_pages", static_cast<double>(control.candidate_pages) },
                                { "allocated_pages",  static_cast<double>(result.requested_pages) }
    },
                            ActionKind::Issued,
                            "storage_issue",
                            cause);
}

bool Storage::finish_check(ProgramRuntime & runtime, std::size_t request, graph::NodeId cause) {
    if (foreground_[request].active) {
        // The final state check may overlap publication. Only this stop point
        // freezes rank progress; tree visibility follows the acceptance response.
        stop(runtime, request, cause);
        return false;
    }
    completion_frontier_[2] = cause;
    if (prefetches_.contains(request)) return accept(request, runtime.now_ns());
    results_[request].ready = true;
    return true;
}

} // namespace markov::trace_graph::planning
