#include "markov/commands/source.hpp"

#include "markov/adapters/source.hpp"
#include "markov/trace_graph/simulation/replay.hpp"
#include "markov/trace_graph/source/build.hpp"
#include "markov/trace_graph/source/context.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace markov::commands {
namespace {

using namespace trace_graph;
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

Json timing_alignment(const source::Observations & observations, const source::SourceGraph & source, const simulation::ReplayResult & replay,
                      graph::Time origin) {
    struct Alignment {
        std::size_t nodes = 0, earlier = 0, later = 0;
        long double absolute_delta_sum = 0;
        graph::Time max_absolute_delta_ns = 0;
        std::size_t largest_node = 0;
    };
    std::map<std::pair<source::ResourceKind, bool>, Alignment> groups;
    for (std::size_t id = 0; id < source.graph.nodes().size(); ++id) {
        const auto & node = source.graph.nodes()[id];
        const bool profiler = node.kind == graph::NodeKind::ProfilerOverhead;
        auto & group = groups[{ observations.resources[node.resource].kind, profiler }];
        const auto delta = replay.starts_ns[id] + origin - source.node_origins[id].begin_ns;
        ++group.nodes;
        group.earlier += delta < 0;
        group.later += delta > 0;
        group.absolute_delta_sum += std::abs(static_cast<long double>(delta));
        if (std::abs(delta) > group.max_absolute_delta_ns) {
            group.max_absolute_delta_ns = std::abs(delta);
            group.largest_node = id;
        }
    }

    auto result = Json::array();
    for (const auto & [identity, group] : groups) {
        const auto [kind, profiler] = identity;
        Json example = nullptr;
        if (group.max_absolute_delta_ns > 0) {
            const auto & node = source.node_origins[group.largest_node];
            if (node.span != std::numeric_limits<source::Index>::max()) {
                const auto & span = observations.spans[node.span];
                example = {
                    {   "node",            group.largest_node },
                    {   "name", observations.names[span.name] },
                    {   "file",              span.record.file },
                    { "record",          span.record.position }
                };
            }
            if (example.is_null()) {
                const auto point = std::ranges::find(source.point_nodes, group.largest_node);
                if (point != source.point_nodes.end()) {
                    const auto & record = observations.points[std::distance(source.point_nodes.begin(), point)];
                    example = {
                        {   "node",              group.largest_node },
                        {   "name", observations.names[record.name] },
                        {   "file",              record.record.file },
                        { "record",          record.record.position }
                    };
                }
            }
        }
        const auto label = kind == source::ResourceKind::CpuThread ? "cpu_thread" : kind == source::ResourceKind::DeviceStream ? "device_stream" : "client";
        result.push_back({
            {                     "resource",                                            label },
            {                    "cost_role", profiler ? "profiler_overhead" : "observed_work" },
            {                        "nodes",                                      group.nodes },
            {                "earlier_nodes",                                    group.earlier },
            {                  "later_nodes",                                      group.later },
            {  "max_absolute_start_delta_ns",                      group.max_absolute_delta_ns },
            {     "largest_interval_example",                               std::move(example) },
            { "mean_absolute_start_delta_ns",           group.absolute_delta_sum / group.nodes }
        });
    }
    return result;
}

Json replay_summary(const source::Observations & observations, const source::SourceGraph & built_source, const simulation::ReplayResult & replay,
                    graph::Time origin) {
    Json requests = Json::array();
    long double observed_sum = 0, absolute_error_sum = 0;
    const source::SourceGraph::HttpEndpoint *formal_first = nullptr, *formal_last = nullptr;
    for (const auto & endpoint : built_source.requests) {
        const auto & request = endpoint.request;
        const auto observed = request.end_ns - request.begin_ns;
        const auto simulated = replay.completions_ns[endpoint.completion] - replay.completions_ns[endpoint.start];
        if (request.formal) {
            if (!formal_first) formal_first = &endpoint;
            formal_last = &endpoint;
            observed_sum += observed;
            absolute_error_sum += std::abs(static_cast<long double>(simulated - observed));
        }
        requests.push_back({
            {             "request",     request.identity },
            {              "formal",       request.formal },
            { "observed_latency_ns",             observed },
            { "replayed_latency_ns",            simulated },
            {     "signed_error_ns", simulated - observed }
        });
    }

    std::size_t gap_nodes = 0;
    graph::Time gap_ns = 0;
    std::size_t wait_nodes = 0;
    graph::Time wait_ns = 0;
    std::size_t root_count = 0;
    for (std::size_t i = 0; i < built_source.graph.nodes().size(); ++i) {
        const auto & node = built_source.graph.nodes()[i];
        if (node.kind == graph::NodeKind::ResidualGap) {
            ++gap_nodes;
            gap_ns += node.duration_ns;
        }
        if (node.kind == graph::NodeKind::ObservedWait) {
            ++wait_nodes;
            wait_ns += node.duration_ns;
        }
        root_count += built_source.graph.incoming_counts()[i] == 0;
    }
    return {
        {                "source_origin_ns",                                                                     origin                                            },
        {                  "executed_nodes",                                                                                                        replay.executed },
        {    "source_observed_root_offsets",                                                                                                             root_count },
        {              "residual_gap_nodes",                                                                                                              gap_nodes },
        {    "residual_gap_duration_sum_ns",                                                                                                                 gap_ns },
        {             "observed_wait_nodes",                                                                                                             wait_nodes },
        {   "observed_wait_duration_sum_ns",                                                                                                                wait_ns },
        {                 "client_requests",                                                                                                    std::move(requests) },
        {     "formal_request_latency_wape",                                             observed_sum > 0 ? Json(absolute_error_sum / observed_sum) : Json(nullptr) },
        {       "observed_formal_window_ns",                      formal_first ? Json(formal_last->request.end_ns - formal_first->request.begin_ns) : Json(nullptr) },
        {       "replayed_formal_window_ns",
         formal_first ? Json(replay.completions_ns[formal_last->completion] - replay.completions_ns[formal_first->start]) : Json(nullptr)                          },
        {                   "qualification", "Observed wall intervals retain waiting and residual gaps; matching source timing does not prove causal completeness." },
        { "unclassified_gaps_are_predicted",                                                                                                                  false },
        {                 "source_requests",                                                                                           observations.requests.size() }
    };
}

} // namespace

nlohmann::json source_report(const adapters::PreparedSource & source) {
    using namespace trace_graph;
    const auto & manifest = source.manifest;
    const auto & context = source.context;
    auto inputs = Json::array();
    for (std::size_t file_id = 0; file_id < manifest.files.size(); ++file_id) {
        const auto & file = manifest.files[file_id];
        inputs.push_back({
            {    "path",                  file.path.string() },
            { "channel",  source::channel_name(file.channel) },
            { "records", source.input_record_counts[file_id] }
        });
    }
    const auto & captured = context.observations();
    const auto & built_source = context.graph();
    const auto begin = Clock::now();
    const auto replay = context.replay();
    const auto finished = Clock::now();
    auto work_descriptions = Json::array();
    std::vector<std::size_t> work_counts(captured.work_descriptions.size(), 0);
    for (const auto & work : captured.work_observations) ++work_counts[work.description];
    for (const auto & work : captured.work_descriptions) {
        work_descriptions.push_back({
            {      "operation",      work.kind.operation },
            { "implementation", work.kind.implementation },
            {   "timing_scope",   work.kind.timing_scope },
            {       "features",            work.features }
        });
    }
    auto replay_report = replay_summary(captured, built_source, replay, context.origin_ns());
    auto alignment_report = timing_alignment(captured, built_source, replay, context.origin_ns());
    const auto summarized = Clock::now();
    auto flow_examples = Json::array();
    for (const auto & flow : captured.unresolved_flows)
        flow_examples.push_back({
            { "identity", flow.identity },
            {   "starts",   flow.starts },
            {     "ends",     flow.ends }
        });
    Json dependency_audit = captured.flow_audit;
    dependency_audit.update(built_source.audit);
    auto dependency_issues = Json::array();
    for (const auto & issue : built_source.dependency_timing_issues) {
        const auto & producer = captured.points[issue.before];
        const auto & consumer = captured.points[issue.after];
        dependency_issues.push_back({
            { "producer",
             { { "file", producer.record.file },
             { "record", producer.record.position },
             { "name", captured.names[producer.name] },
             { "time_ns", producer.time_ns } }   },
            { "consumer",
             { { "file", consumer.record.file },
             { "record", consumer.record.position },
             { "name", captured.names[consumer.name] },
             { "time_ns", consumer.time_ns } }   },
            { "delta_ns",          issue.delta_ns }
        });
    }
    return {
        {                         "status",                                                       "source_graph_replayed"                                           },
        {                       "manifest",                                                                                                   manifest.path.string() },
        {        "capture_origin_manifest",                      manifest.capture_origin_manifest ? Json(manifest.capture_origin_manifest->string()) : Json(nullptr) },
        {    "capture_completeness_proven",                                                                                                                    false },
        {                     "cost_basis",                                                            "observed_exclusive_wall_intervals_and_unclassified_cpu_gaps" },
        {                         "inputs",                                                                                                        std::move(inputs) },
        {                 "observed_spans",                                                                                                    captured.spans.size() },
        {                "observed_points",                                                                                                   captured.points.size() },
        {                    "graph_nodes",                                                                                        built_source.graph.nodes().size() },
        {                    "graph_edges",                                                                                        built_source.graph.edges().size() },
        {       "native_observation_roles",                                                                                                      source.native_roles },
        {              "work_descriptions",                                                                                             std::move(work_descriptions) },
        {         "work_description_scope",                                                                                          "observed_linear_geometry_only" },
        {        "work_observation_counts",                                                                                                   std::move(work_counts) },
        {              "work_observations",                                                                                        captured.work_observations.size() },
        {                  "record_counts",                                                                                                          captured.counts },
        {               "dependency_audit",                                                                                              std::move(dependency_audit) },
        {       "unresolved_flow_examples",                                                                                                 std::move(flow_examples) },
        {              "dependency_timing", { { "max_violation_ns", built_source.max_dependency_clock_violation_ns }, { "examples", std::move(dependency_issues) } } },
        {                         "replay",                                                                                                 std::move(replay_report) },
        {               "timing_alignment",                                                                                              std::move(alignment_report) },
        {                     "timing_sec",
         { { "read_and_interpret", source.read_and_interpret_sec },
         { "build", source.build_sec },
         { "replay", std::chrono::duration<double>(finished - begin).count() },
         { "summary", std::chrono::duration<double>(summarized - finished).count() } }                                                                              },
        {  "dependency_correctness_proven",                                                                                                                    false },
        { "normal_runtime_accuracy_proven",                                                                                                                    false }
    };
}

} // namespace markov::commands
