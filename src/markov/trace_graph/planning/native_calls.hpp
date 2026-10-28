#pragma once

#include "markov/trace_graph/planning/compute.hpp"
#include "markov/trace_graph/planning/program.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace markov::trace_graph::planning {

enum class CallKind { Cpu, Queued, Readback, HostUpload };

struct KernelPreparation {
    Work specialization, compilation, first_load, load;
};

struct NativeCall {
    Work work;
    CallKind kind;
    std::vector<Work> launches;
    std::optional<Work> synchronization = std::nullopt;
    std::optional<std::size_t> source_operation = std::nullopt;
    std::optional<KernelPreparation> preparation = std::nullopt;

    // A mixed sequence uses the incoming producer here and the frame stream
    // for its other calls; one source binding cannot collapse both roles.
    bool incoming_stream = false;
    std::optional<Work> host_transfer = std::nullopt;
};

struct NativeSequence {
    std::vector<NativeCall> calls;
    std::optional<std::string> issue;
};

enum class KernelCacheInitial { Empty, Prepared };
enum class KernelUse { Prepared, FirstLoad, Load };

// Planned preparation belongs to one target. A cost failure discards that
// target; these declarations never mark a real runtime cache as initialized.
class PreparationCache {
public:
    explicit PreparationCache(std::optional<KernelCacheInitial> initial) : initial_(initial) {}
    std::optional<KernelCacheInitial> initial() const { return initial_; }
    KernelUse use(const KernelPreparation & preparation, std::size_t caller);
    bool runtime_initialized(std::size_t caller) const { return initial_ == KernelCacheInitial::Prepared || loaded_.contains(caller); }
    std::size_t specializations() const;
    std::size_t first_loads() const;
    std::size_t runtime_initializations() const { return loaded_.size(); }

private:
    std::optional<KernelCacheInitial> initial_;
    std::map<std::size_t, std::set<Work>> prepared_;
    std::map<std::size_t, std::set<Work>> loaded_;
};

// Framework rules describe changing work; the common compiler owns the caller,
// worker and device lifecycle. Neither interface sees costs or a second clock.
class CallSequence {
public:
    virtual ~CallSequence() = default;
    virtual NativeSequence describe(Phase phase, const FrameInput & input, std::span<const Work> work = {}) const = 0;
};

struct CallResources {
    std::size_t caller, worker, device;
    bool operator==(const CallResources &) const = default;
};

class NativePlan {
public:
    // Ordered tensor work may change device streams while retaining its
    // producer frontier. Caller/worker ownership remains with the same queues.
    bool extend(std::span<const NativeCall> calls, CallResources resources, PreparationCache & preparations);
    void source_cpu_gap(std::size_t interval, std::size_t caller);
    PlannedMutation mutation(simulation::Component owner, graph::NodeId begin, simulation::Action completion, std::optional<graph::NodeId> cause) const;
    std::size_t operations() const { return steps_.size(); }
    std::size_t delays() const;
    std::optional<std::size_t> producer_device() const { return device_ ? std::optional(device_->device) : std::nullopt; }

private:
    graph::NodeId append(const Work & work, const char * scope, std::size_t resource, std::optional<graph::NodeId> & tail, bool delay = false);
    void cpu(const Work & work, const char * service, const char * residual, std::size_t resource, std::optional<graph::NodeId> & tail);
    void depend(std::optional<graph::NodeId> before, graph::NodeId after);

    struct Step {
        std::variant<Work, SourceCpuGap> cost;
        std::size_t resource;
        bool delay;
        std::optional<std::size_t> source_operation = std::nullopt;
    };
    std::vector<Step> steps_;
    std::vector<graph::Edge> edges_;
    std::optional<graph::NodeId> caller_, worker_;
    struct DeviceProducer {
        graph::NodeId completion;
        std::size_t device;
    };
    std::optional<DeviceProducer> device_;
};

} // namespace markov::trace_graph::planning
