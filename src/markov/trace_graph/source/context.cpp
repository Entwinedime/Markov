#include "markov/trace_graph/source/context.hpp"
#include "markov/trace_graph/source/details.hpp"
#include "markov/trace_graph/source/regions.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

namespace markov::trace_graph::source {

SourceContext::SourceContext(Observations && observations) : observations_(std::move(observations)), origin_ns_(std::numeric_limits<Time>::max()) {
    client_resource_ = observations_.resource(ResourceKind::Client, "http_client");
    for (const auto & span : observations_.spans) origin_ns_ = std::min(origin_ns_, span.begin_ns);
    for (const auto & point : observations_.points) origin_ns_ = std::min(origin_ns_, point.time_ns);
    for (const auto & request : observations_.requests) origin_ns_ = std::min(origin_ns_, request.begin_ns);

    frame_operations_.resize(observations_.compute_frames.size());
    for (Index id = 0; id < observations_.work_observations.size(); ++id) frame_operations_[observations_.work_observations[id].frame].push_back(id);
    for (auto & operations : frame_operations_) {
        std::ranges::stable_sort(operations, {}, [&](Index id) { return observations_.work_observations[id].begin_ns; });
    }
    for (Index frame = 0; frame < observations_.compute_frames.size(); ++frame) {
        for (const auto request : observations_.compute_frames[frame].requests) request_frames_[observations_.names[request]].push_back(frame);
    }
    frame_results_.resize(observations_.compute_frames.size());
    for (Index id = 0; id < observations_.batch_results.size(); ++id) {
        auto & result = frame_results_.at(observations_.batch_results[id].frame);
        if (result) throw std::runtime_error("Multiple batch results refer to one compute frame");
        result = id;
    }

    observations_.release_intake_indexes();
}

SourceContext::~SourceContext() = default;

SourceContext::SourceContext(SourceContext && other) noexcept
    : observations_(std::move(other.observations_)),
      origin_ns_(other.origin_ns_),
      client_resource_(other.client_resource_),
      frame_operations_(std::move(other.frame_operations_)),
      request_frames_(std::move(other.request_frames_)),
      frame_results_(std::move(other.frame_results_)),
      regions_(std::move(other.regions_)) {
    other.details_.reset();
}

SourceContext & SourceContext::operator=(SourceContext && other) noexcept {
    if (this == &other) return *this;

    details_.reset();
    detailed_preparation_sec_ = 0;
    other.details_.reset();
    observations_ = std::move(other.observations_);
    origin_ns_ = other.origin_ns_;
    client_resource_ = other.client_resource_;
    frame_operations_ = std::move(other.frame_operations_);
    request_frames_ = std::move(other.request_frames_);
    frame_results_ = std::move(other.frame_results_);
    regions_ = std::move(other.regions_);
    return *this;
}

const SourceDetails & SourceContext::details() const {
    if (!details_) {
        const auto begin = std::chrono::steady_clock::now();
        details_ = std::make_unique<SourceDetails>(*this);
        detailed_preparation_sec_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    }
    return *details_;
}

const SourceGraph & SourceContext::graph() const { return details().graph_; }

const Regions & SourceContext::regions() const {
    if (!regions_) regions_ = std::make_unique<Regions>(*this);
    return *regions_;
}

const DeviceService & SourceContext::device_service(Index work) const { return details().device_services_[work]; }

const CpuInterval & SourceContext::cpu_work_interval(Index work) const { return details().cpu_work_intervals_[work]; }

const std::optional<planning::CallResources> & SourceContext::compute_resources(Index work) const { return details().compute_resources_[work]; }

const SourceSequence & SourceContext::sequence(Index id) const { return details().sequences_[id]; }

std::span<const Index> SourceContext::frame_sequences(Index frame) const { return details().frame_sequences_[frame]; }

std::span<const Index> SourceContext::result_sequences(Index frame) const { return details().result_sequences_[frame]; }

std::span<const Index> SourceContext::lookup_sequences(Index frame) const { return details().lookup_sequences_[frame]; }

std::span<const NativeWorkRegion> SourceContext::native_work() const { return details().native_work_; }

std::span<const WorkOrder> SourceContext::frame_queue_order(Index frame) const { return details().frame_queue_order_[frame]; }

std::span<const Index> SourceContext::frame_queue_issues(Index frame) const { return details().frame_queue_issues_[frame]; }

std::span<const Index> SourceContext::frame_collectives(Index frame) const { return details().frame_collectives_[frame]; }

std::span<const FrameCall> SourceContext::frame_calls(Index frame) const { return details().frame_calls_[frame]; }

bool SourceContext::frame_cpu_order_supported(Index frame) const { return details().frame_cpu_order_supported_[frame]; }

std::span<const ObservedRegion> SourceContext::cpu_gaps() const { return details().cpu_gaps_; }

bool SourceContext::cpu_gap_has_unmodeled_work(Index interval) const { return details().cpu_gap_native_submissions_[interval]; }

void SourceContext::bind_native_sources(Index sequence, std::span<planning::NativeCall> calls, planning::CallResources resources) const {
    details().bind_native_sources(sequence, calls, resources);
}

std::span<const Index> SourceContext::request_frames(std::string_view request) const {
    const auto found = request_frames_.find(request);
    return found == request_frames_.end() ? std::span<const Index>{} : found->second;
}

const BatchResult * SourceContext::frame_result(Index frame) const {
    const auto result = frame_results_[frame];
    return result ? &observations_.batch_results[*result] : nullptr;
}

simulation::ExecutionResult SourceContext::replay() const {
    const auto & source = graph();
    std::vector<Time> available(source.graph.nodes().size(), 0);
    for (Index i = 0; i < available.size(); ++i) {
        if (source.graph.incoming_counts()[i] == 0) available[i] = source.node_origins[i].begin_ns - origin_ns_;
    }
    simulation::Execution execution(source.graph, available);
    execution.run();
    return execution.take_result();
}

} // namespace markov::trace_graph::source
