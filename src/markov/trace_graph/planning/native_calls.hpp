#pragma once

#include "markov/trace_graph/planning/compute.hpp"
#include "markov/trace_graph/planning/program.hpp"

#include <cstddef>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace markov::trace_graph::planning {

// HostCall pays one complete API response; scalar readback retains its pre/post-sync costs.
enum class CallKind { Cpu, HostCall, Queued, Readback, HostResponse, HostUpload };
enum class DeviceControl { None, Record, Wait };
enum class NativeProducer { Caller, Device };
enum class ResultControl {
    UpdatePrefix,
    CommitPrefix,
    MatchPrefix,
    UpdateDuplicates,
    CommitDuplicates,
    ReleaseTail,
    ReleaseGroup,
    ConvertReleasedIndices,
    PublishReleasedPages
};

struct ResultControlPoint {
    ResultControl operation;
    // Immediate releases own a group; deferred releases share the batch group.
    ResultControl release_group = ResultControl::ReleaseGroup;
};

struct KernelPreparation {
    Work specialization, compilation, first_load, load;
    bool operator==(const KernelPreparation &) const = default;
};

struct NativeLaunchSources {
    std::optional<std::size_t> worker_before, device;
};

struct NativeSources {
    std::optional<std::size_t> caller_before, caller_after, worker_before, worker_after;
    std::optional<std::size_t> dispatch;
    std::vector<NativeLaunchSources> launches;
    std::vector<std::optional<std::size_t>> caller_preparations;
};

struct CallerPreparation {
    Work work;
    std::string observation_name;
    bool operator==(const CallerPreparation &) const = default;
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

    // Framework work between native calls belongs to the caller envelope;
    // it must not change the worker or device work used for cost queries.
    std::optional<Work> caller_work = std::nullopt;

    // Optional native observation identity, interpreted by the adapter. It is
    // evidence for a source sequence, never a cost key or target branch rule.
    std::string queue_name = {};
    NativeSources sources = {};
    DeviceControl device_control = DeviceControl::None;
    std::optional<Work> worker_first_use = std::nullopt;

    // Submission can be unchanged while the device processes a different shape.
    std::optional<Features> worker_features = std::nullopt;
    Work worker_work(const Work & operation) const;
    std::vector<CallerPreparation> caller_preparations = {};
};

struct NativeSequence {
    std::vector<NativeCall> calls;
    std::optional<std::string> issue;
    std::optional<NativeProducer> result_ready = std::nullopt;
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
    bool first_use(const Work & preparation, std::size_t resource);
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

using NativeFrontier = std::map<std::size_t, graph::NodeId>;

// A caller turn can span graph mutations and synchronization waits. Grants are
// causal ports; service durations and resource execution remain in the runtime.
class CallerSchedule {
public:
    void acquire(ProgramRuntime & runtime, std::size_t caller, simulation::Component owner, simulation::Action granted, std::optional<graph::NodeId> cause);
    void release(ProgramRuntime & runtime, std::size_t caller, simulation::Component owner, graph::NodeId completion);
    void handoff(ProgramRuntime & runtime, std::size_t caller, simulation::Component previous, simulation::Component next, simulation::Action granted,
                 graph::NodeId cause);

private:
    struct Request {
        simulation::Component owner;
        simulation::Action granted;
        std::optional<graph::NodeId> cause;
    };
    struct Lane {
        std::optional<simulation::Component> owner;
        std::optional<graph::NodeId> completion;
        std::deque<Request> pending;
    };
    void dispatch(ProgramRuntime & runtime, std::size_t caller, Lane & lane);
    std::map<std::size_t, Lane> lanes_;
};

struct ExecutionQueues {
    NativeFrontier native;
    CallerSchedule callers;
};

struct NativeEvents {
    simulation::Action complete;
    std::optional<simulation::Action> submitted = std::nullopt;
    std::optional<simulation::Action> produced = std::nullopt;
};

class NativePlan {
public:
    // Ordered tensor work may change device streams while retaining its
    // producer frontier. Caller/worker ownership remains with the same queues.
    bool extend(const NativeSequence & sequence, CallResources resources, PreparationCache & preparations);
    void source_cpu_gap(std::size_t interval, std::size_t caller);
    PlannedMutation mutation(simulation::Component owner, graph::NodeId begin, NativeEvents events, std::optional<graph::NodeId> cause,
                             const NativeFrontier & preceding) const;
    NativeFrontier frontier(graph::NodeId begin) const;
    std::size_t operations() const { return steps_.size(); }
    std::size_t delays() const;
    std::optional<std::size_t> producer_device() const { return device_ ? std::optional(device_->device) : std::nullopt; }
    bool publishes_result() const { return result_.has_value(); }
    std::optional<graph::NodeId> result_producer(graph::NodeId begin) const { return result_ ? std::optional(begin + *result_) : std::nullopt; }

private:
    graph::NodeId append(const Work & work, const char * scope, std::size_t resource, std::optional<graph::NodeId> & tail, bool delay = false);
    void cpu(const Work & work, const char * service, const char * residual, std::size_t resource, std::optional<graph::NodeId> & tail,
             std::optional<std::size_t> source = std::nullopt);
    void depend(std::optional<graph::NodeId> before, graph::NodeId after);

    struct Step {
        std::variant<Work, SourceCpuGap> cost;
        std::size_t resource;
        bool delay;
        std::optional<std::size_t> source_operation = std::nullopt;
        std::optional<std::size_t> source_native_work = std::nullopt;
    };
    std::vector<Step> steps_;
    std::vector<graph::Edge> edges_;
    std::vector<std::pair<graph::NodeId, std::size_t>> preceding_waits_;
    std::optional<graph::NodeId> caller_, worker_;
    struct DeviceProducer {
        graph::NodeId completion;
        std::size_t device;
    };
    std::optional<DeviceProducer> device_;
    std::optional<graph::NodeId> result_;
};

} // namespace markov::trace_graph::planning
