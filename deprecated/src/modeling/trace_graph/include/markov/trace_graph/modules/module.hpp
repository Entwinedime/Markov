/**
 * @file
 * @brief Business execution interface for TraceGraph what-if modules.
 */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"

#include <string_view>

namespace markov::trace_graph::modules {

/**
 * @brief Common C++ interface for ordered what-if modules.
 *
 * A module may inspect trace facts, retain model-local state, and mutate DAG nodes,
 * edges, or durations. It must not rewrite source traces or mix diagnostics into the
 * default business result. The CLI composition root owns module ordering.
 */
class SimulationModule {
public:
    virtual ~SimulationModule() = default;

    /** @brief Returns the stable registry and diagnostics name without allocating. */
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /** @brief Applies this module once to the graph at its configured pipeline position. */
    virtual void apply(core::DagGraph & graph) = 0;

    /** @brief Reports whether a diagnostics summary is available after apply(). */
#ifdef DEBUG
    [[nodiscard]] virtual bool has_summary() const { return false; }
#endif
};

} // namespace markov::trace_graph::modules
