#pragma once

#include "markov/trace_graph/planning/compute.hpp"
#include "markov/trace_graph/planning/program.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace markov::trace_graph::planning {

enum class CallKind { Cpu, Queued, Readback };

struct NativeCall {
    Work work;
    CallKind kind;
    std::vector<Work> launches;
    std::optional<Work> synchronization = std::nullopt;
    std::optional<std::size_t> source_operation = std::nullopt;
};

// Framework rules describe changing work; the common compiler owns the caller,
// worker and device lifecycle. Neither interface sees costs or a second clock.
class CallSequence {
public:
    virtual ~CallSequence() = default;
    virtual std::vector<NativeCall> describe(Phase phase, const AttentionInput & input, std::span<const Work> work = {}) const = 0;
};

struct CallResources {
    std::size_t caller, worker, device;
    bool operator==(const CallResources &) const = default;
};

class NativePlan {
public:
    void extend(std::span<const NativeCall> calls, CallResources resources);
    PlannedMutation mutation(simulation::Component owner, graph::NodeId begin, simulation::Action completion, std::optional<graph::NodeId> cause) const;
    std::size_t operations() const { return work_.size(); }
    std::size_t delays() const;

private:
    graph::NodeId append(const Work & work, const char * scope, std::size_t resource, std::optional<graph::NodeId> & tail, bool delay = false);
    void cpu(const Work & work, const char * service, const char * residual, std::size_t resource, std::optional<graph::NodeId> & tail);
    void depend(std::optional<graph::NodeId> before, graph::NodeId after);

    struct Step {
        std::size_t resource;
        bool delay;
        std::optional<std::size_t> source_operation = std::nullopt;
    };
    std::vector<Work> work_;
    std::vector<Step> steps_;
    std::vector<graph::Edge> edges_;
    std::optional<graph::NodeId> caller_, worker_, device_;
};

} // namespace markov::trace_graph::planning
