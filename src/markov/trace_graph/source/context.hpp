#pragma once

#include "markov/trace_graph/simulation/execution.hpp"
#include "markov/trace_graph/source/build.hpp"
#include "markov/trace_graph/source/observations.hpp"

namespace markov::trace_graph::source {

// Own source observations and their graph once. Consumers borrow const views;
// target state and corrected cost views cannot mutate the original capture.
class SourceContext {
public:
    explicit SourceContext(Observations && observations);
    SourceContext(const SourceContext &) = delete;
    SourceContext & operator=(const SourceContext &) = delete;
    SourceContext(SourceContext &&) = default;
    SourceContext & operator=(SourceContext &&) = default;

    const Observations & observations() const { return observations_; }
    const SourceGraph & graph() const { return graph_; }
    Time origin_ns() const { return origin_ns_; }
    simulation::ExecutionResult replay() const;

private:
    Observations observations_;
    SourceGraph graph_;
    Time origin_ns_;
};

} // namespace markov::trace_graph::source
