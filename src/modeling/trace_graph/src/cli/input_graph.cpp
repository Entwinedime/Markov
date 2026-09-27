#include "markov/trace_graph/cli/input_graph.hpp"

#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/frontend/trace_normalizer.hpp"

#include <algorithm>
#include <future>
#include <limits>
#include <stdexcept>
#include <utility>

namespace markov::trace_graph::cli {
namespace {

core::DagGraph build_one(io::ManifestTraceInput input, size_t index, size_t threads) {
    frontend::normalize_trace_events(input.events);
    auto graph = core::DagBuilder(threads).build(std::move(input.events), static_cast<int>(index));
#ifdef DEBUG
    uint64_t begin = std::numeric_limits<uint64_t>::max(), end = 0;
    for (const auto & node : graph.nodes()) {
        const auto & event = graph.event_for_node(node.id);
        begin = std::min(begin, event.ts);
        end = std::max(end, core::checked_add_u64(event.ts, event.dur, "trace timestamp overflow while measuring observed E2E"));
    }
    graph.set_real_e2e_time(end > begin ? end - begin : 0);
#endif
    graph.set_input_contracts(std::move(input.input_contracts));
    graph.set_context_events(std::move(input.context_events));
    graph.set_prelude_context_events(std::move(input.prelude_context_events));
    graph.set_tail_context_events(std::move(input.tail_context_events));
    return graph;
}

} // namespace

core::DagGraph build_input_graph(std::vector<io::ManifestTraceInput> inputs, size_t thread_budget) {
    if (!inputs.empty() && inputs.size() - 1 > static_cast<size_t>(std::numeric_limits<int>::max()))
        throw std::overflow_error("Logical trace input index exceeds GPU ID range");

    const size_t concurrency = std::max<size_t>(1, std::min(thread_budget, inputs.size()));
    const size_t build_threads = std::max<size_t>(1, thread_budget / concurrency);
    std::vector<core::DagGraph> graphs(inputs.size());
    for (size_t begin = 0; begin < inputs.size(); begin += concurrency) {
        if (concurrency == 1) {
            graphs[begin] = build_one(std::move(inputs[begin]), begin, build_threads);
            continue;
        }

        const size_t end = std::min(inputs.size(), begin + concurrency);
        std::vector<std::future<core::DagGraph>> futures;
        for (size_t index = begin; index < end; ++index) {
            futures.push_back(
                std::async(std::launch::async, [&inputs, index, build_threads] { return build_one(std::move(inputs[index]), index, build_threads); }));
        }
        // Completion order must not change logical input identity or merge order.
        for (size_t index = begin; index < end; ++index) graphs[index] = futures[index - begin].get();
    }
    return core::DagGraph::merge(std::move(graphs));
}

} // namespace markov::trace_graph::cli
