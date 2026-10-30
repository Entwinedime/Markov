#pragma once

#include "markov/trace_graph/planning/program.hpp"

#include <span>
#include <vector>

namespace markov::trace_graph::planning {

// Closed synchronous work has no intermediate external consumer. Controller
// remainder may be grouped once; phase intervals already include device waits.
struct SerialRequest {
    std::size_t source_request;
    std::size_t before_execution, after_execution;
    std::vector<Work> work;
};

struct SerialRequestPorts {
    graph::NodeId start, response;
};

class SerialProgram final : public Program {
public:
    explicit SerialProgram(std::span<const SerialRequest> requests) : requests_(requests) {}
    void start(ProgramRuntime & runtime) override;
    void on_event(ProgramRuntime &, const simulation::Event &) override { complete_ = true; }
    bool complete() const override { return complete_; }
    std::span<const SerialRequestPorts> ports() const { return ports_; }
    static constexpr std::size_t resource_count = 1;

private:
    std::span<const SerialRequest> requests_;
    std::vector<SerialRequestPorts> ports_;
    bool complete_ = false;
};

} // namespace markov::trace_graph::planning
