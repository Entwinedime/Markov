#pragma once

#include "markov/trace_graph/planning/compute_plan.hpp"
#include "markov/trace_graph/planning/program.hpp"
#include "markov/trace_graph/source/context.hpp"

#include <span>

namespace markov::trace_graph::planning {

// Execute the known recipe subset on its observed service resources. This is
// not a full frame: cross-stream data, communication and cache dependencies are absent.
class ComputeProgram final : public Program {
public:
    ComputeProgram(const source::SourceContext & source, const ComputePlan & plan, source::Index frame) : source_(source), plan_(plan), frame_(frame) {}

    std::span<const source::Index> unresolved_queues() const { return source_.frame_queue_issues(frame_); }
    void start(ProgramRuntime & runtime) override;
    void on_event(ProgramRuntime & runtime, const simulation::Event & event) override;
    bool complete() const override { return complete_; }

private:
    const source::SourceContext & source_;
    const ComputePlan & plan_;
    source::Index frame_;
    bool complete_ = false;
};

} // namespace markov::trace_graph::planning
