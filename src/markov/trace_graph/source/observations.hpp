#pragma once

#include "markov/trace_graph/planning/compute.hpp"
#include "markov/trace_graph/planning/native_calls.hpp"
#include "markov/trace_graph/planning/token_paths.hpp"
#include "markov/trace_graph/planning/work.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace markov::trace_graph::source {

using Time = std::int64_t;
using Index = std::size_t;

enum class ResourceKind { CpuThread, DeviceStream, Client };

struct Resource {
    ResourceKind kind;
    std::string identity;
};

struct RecordRef {
    Index file;
    Index position;
};

enum class SpanRole { Service, Wait, ProfilerOverhead, StreamWait, EventWait };

struct ObservedSpan {
    Time begin_ns;
    Time end_ns;
    Index resource;
    Index name;
    RecordRef record;
    SpanRole role = SpanRole::Service;
};

struct ObservedPoint {
    Time time_ns;
    Index resource;
    Index name;
    RecordRef record;
};

enum class DependencyKind { Flow, Submission, Synchronization };

struct ObservedFlow {
    std::string identity;
    Index point;
    bool start;
    DependencyKind kind = DependencyKind::Flow;
};

struct ObservedDependency {
    Index before;
    Index after;
    DependencyKind kind;
};

struct FlowDiagnostic {
    std::string identity;
    std::size_t starts;
    std::size_t ends;
};

struct RequestWindow {
    std::string identity;
    Time begin_ns;
    Time end_ns;
    bool formal;
    std::optional<Index> received = std::nullopt;
    std::optional<Index> sent = std::nullopt;
};

enum class BarrierKind { DrainHiCache };

struct WorkloadBarrier {
    // The control occurs before this request index; size() means after the last.
    Index before_request;
    BarrierKind kind;
    RecordRef record;
};

struct ObservedWork {
    Index description;
    Index resource;
    Time begin_ns;
    Time end_ns;
    RecordRef record;
    Index frame;
    std::optional<Index> layer;
};

struct WorkDescription {
    // One native call owns an ordered recipe, not one cost per CPU envelope.
    // Keep its captured expansion and fixed geometry for later variable binding.
    std::vector<planning::Work> observed;
    std::vector<planning::ComputePart> recipe;
    std::unique_ptr<const planning::CallSequence> calls;
};

struct ObservedRegion {
    Index resource;
    Time begin_ns, end_ns;
    RecordRef record;
};

struct RequestExecutionRegion {
    Index request;
    ObservedRegion region;
};

// Measured semantic responses. Request refers to the name pool; background work
// has no request. Grouped noncontiguous remainders cannot establish wall order.
struct ObservedCost {
    std::optional<Index> request;
    Index iteration;
    std::optional<Index> participant;
    std::string_view port;
    planning::Work work;
    Time duration_ns;
    ObservedRegion region;
    std::optional<std::vector<std::int64_t>> data = std::nullopt;
    bool contiguous = true;
};

struct NativeQueueCall {
    Index name;
    ObservedRegion submission, execution;
};

struct NativeRuntimeCall {
    Index implementation;
    ObservedRegion region;
    bool code_preparation = false;
    bool device_submission = false;
};

struct ObservedDeviceControl {
    planning::DeviceControl kind;
    ObservedRegion submitted, command;
    std::optional<Time> producer_completed_ns = std::nullopt;
};

struct ObservedCallSequence {
    ObservedRegion region;
    std::unique_ptr<const planning::CallSequence> definition;
    std::string role;
    std::optional<planning::ResultControlPoint> control = std::nullopt;
    // Source geometry binds observations; target controls use current state.
    std::vector<planning::Work> source_work = {};
};

struct ComputeFrame {
    planning::Phase phase;
    std::vector<Index> requests; // Indices into Observations::names, not requests.
    std::vector<double> sequence_tokens;
    std::optional<std::vector<double>> query_tokens;
    std::optional<double> page_tokens;
    RecordRef record;
    std::vector<ObservedCallSequence> preparations;
    std::vector<ObservedCallSequence> lookups = {};
    std::optional<ObservedRegion> preparation_region = std::nullopt;
    std::optional<Index> participant = std::nullopt;
};

struct CollectiveGroup {
    std::string identity;
    std::vector<Index> members;
};

struct ObservedCollective {
    Index group, rank, sequence, frame, resource;
    Time begin_ns, end_ns;
    RecordRef record;
    planning::Work work;
    planning::RowBinding rows;
};

struct ResultCopy {
    ObservedRegion region;
    std::vector<Index> token_shape;
    std::string token_dtype, token_device;
    Index token_bytes;
    bool additional_outputs;
};

struct ProcessedResult {
    ObservedRegion region;
    std::vector<Index> output_tokens_before, output_tokens_after;
    std::vector<bool> finished_before, finished_after;
    std::vector<Index> controls = {};
    std::vector<ObservedCallSequence> callers = {};
};

struct HostControl {
    ObservedRegion region;
    planning::Work work;
    Time thread_cpu_ns;
    std::optional<Index> request;
};

struct ResultSend {
    ObservedRegion region;
    std::vector<Index> requests;
};

struct BatchResult {
    Index frame;
    ObservedRegion run;
    bool overlap;
    std::optional<ResultCopy> copy;
    std::optional<ProcessedResult> processed;
    std::vector<ResultSend> final_sends;
};

enum class TokenRole { LookupInput, Prompt, Committed };

struct TokenObservation {
    planning::PrefixId path;
    RecordRef record;
};

struct RequestTokens {
    std::optional<TokenObservation> lookup_input;
    std::optional<TokenObservation> prompt;
    std::optional<TokenObservation> committed;
};

// Source-only observations. No target policy, regression or mutable execution
// state lives here. Resource and name IDs remain stable for the capture lifetime.
class Observations {
public:
    Index name(const std::string & value);
    Index resource(ResourceKind kind, const std::string & identity);
    Index point(ObservedPoint value);
    void span(ObservedSpan value);
    void alias_resource(Index from, Index to);
    Index canonical_resource(Index resource) const;
    void normalize_resources();
    void resolve_flows();
    void release_intake_indexes();
    Index describe_work(std::vector<planning::Work> work, std::vector<planning::ComputePart> recipe, std::string caller, planning::Features call_parameters);
    void observe_tokens(const std::string & request, TokenRole role, planning::PrefixId path, RecordRef record);

    std::vector<std::string> names;
    std::vector<Resource> resources;
    std::vector<ObservedSpan> spans;
    std::vector<ObservedPoint> points;
    std::vector<ObservedFlow> flows;
    std::vector<ObservedDependency> dependencies;
    std::vector<NativeQueueCall> native_queue_calls;
    std::vector<NativeRuntimeCall> native_runtime_calls;
    std::vector<ObservedDeviceControl> device_controls;
    std::map<std::string, std::size_t> flow_audit;
    std::vector<FlowDiagnostic> unresolved_flows;
    std::vector<RequestWindow> requests;
    std::vector<RequestExecutionRegion> request_executions;
    std::vector<ObservedCost> cost_observations;
    std::vector<WorkloadBarrier> workload_barriers;
    std::vector<WorkDescription> work_descriptions;
    std::vector<ObservedWork> work_observations;
    std::vector<ComputeFrame> compute_frames;
    std::vector<CollectiveGroup> collective_groups;
    std::vector<ObservedCollective> collectives;
    std::vector<BatchResult> batch_results;
    std::vector<HostControl> host_controls;
    planning::TokenPaths token_paths;
    std::map<std::string, RequestTokens> request_tokens;
    std::map<std::string, std::size_t> counts;

private:
    std::unordered_map<std::string, Index> name_ids_;
    std::unordered_map<std::string, Index> resource_ids_;
    std::unordered_map<Index, Index> resource_aliases_;
    std::map<std::tuple<std::vector<planning::ComputePart>, std::vector<planning::Work>, std::string, planning::Features>, Index> work_ids_;
};

} // namespace markov::trace_graph::source
