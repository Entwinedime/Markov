#include "markov/commands/source.hpp"

#include "markov/adapters/source.hpp"
#include "markov/commands/costs.hpp"
#include "markov/trace_graph/planning/compute_plan.hpp"
#include "markov/trace_graph/source/build.hpp"
#include "markov/trace_graph/source/context.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace markov::commands {
namespace {

using namespace trace_graph;
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

Json cpu_interval_summary(const source::SourceContext & source) {
    struct Summary {
        std::size_t calls = 0;
        source::Time wall_ns = 0;
        source::CpuInterval interval;

        void include(const source::CpuInterval & value, source::Time wall) {
            ++calls;
            wall_ns += wall;
            interval.service_ns += value.service_ns;
            interval.wait_ns += value.wait_ns;
            interval.residual_ns += value.residual_ns;
            interval.instrumentation_ns += value.instrumentation_ns;
            interval.unobserved_ns += value.unobserved_ns;
        }
    };
    const auto & observed = source.observations();
    std::vector<Summary> compute(observed.work_descriptions.size());
    std::vector<std::size_t> native_bindings(compute.size(), 0);
    for (source::Index id = 0; id < observed.work_observations.size(); ++id) {
        const auto & work = observed.work_observations[id];
        compute[work.description].include(source.cpu_work_interval(id), work.end_ns - work.begin_ns);
        native_bindings[work.description] += source.compute_resources(id).has_value();
    }
    Summary metadata;
    std::size_t metadata_rules = 0, metadata_queues = 0;
    for (source::Index frame = 0; frame < observed.compute_frames.size(); ++frame) {
        const auto & regions = observed.compute_frames[frame].metadata;
        const auto intervals = source.metadata_intervals(frame);
        for (std::size_t i = 0; i < regions.size(); ++i) metadata.include(intervals[i], regions[i].region.end_ns - regions[i].region.begin_ns);
        for (const auto & call : regions) metadata_rules += call.definition != nullptr;
        for (const auto & resources : source.metadata_resources(frame)) metadata_queues += resources.has_value();
    }
    const auto describe = [](const Summary & summary) -> Json {
        return {
            {              "calls",                       summary.calls },
            {            "wall_ns",                     summary.wall_ns },
            {         "service_ns",         summary.interval.service_ns },
            {   "observed_wait_ns",            summary.interval.wait_ns },
            {    "residual_gap_ns",        summary.interval.residual_ns },
            { "instrumentation_ns", summary.interval.instrumentation_ns },
            {      "unobserved_ns",      summary.interval.unobserved_ns }
        };
    };
    auto descriptions = Json::array();
    for (std::size_t id = 0; id < compute.size(); ++id) {
        auto item = describe(compute[id]);
        item["work_description"] = id;
        item["native_resource_bindings"] = native_bindings[id];
        item["native_call_rule"] = observed.work_descriptions[id].calls != nullptr;
        descriptions.push_back(std::move(item));
    }
    return {
        {              "compute_calls",std::move(descriptions)                                       },
        {         "attention_metadata",      describe(metadata) },
        {             "metadata_rules",          metadata_rules },
        { "metadata_resource_bindings",         metadata_queues },
        {              "qualification",
         "Source caller intervals only: exclusive observed service, waits, gaps, and uncovered time are kept separate. "
         "Profiler correction is not applied; these are neither normal CPU costs nor a complete request partition. Device/worker work is not charged "
         "here."                                               }
    };
}

Json device_service_summary(const source::SourceContext & source) {
    struct Summary {
        std::map<std::string, std::size_t> statuses;
        std::size_t nodes = 0;
        graph::Time minimum_ns = std::numeric_limits<graph::Time>::max(), maximum_ns = 0;
        std::optional<source::Index> example;
    };
    const auto & observed = source.observations();
    std::vector<Summary> summaries(observed.work_descriptions.size());
    std::map<std::string, std::size_t> totals;
    for (source::Index id = 0; id < observed.work_observations.size(); ++id) {
        const auto & service = source.device_service(id);
        const char * status = service.available() ? "bound" : "no_observed_launch";
        if (service.issue) {
            switch (*service.issue) {
            case source::ServiceIssue::SharedProbe:
                status = "shared_probe";
                break;
            case source::ServiceIssue::AmbiguousOwner:
                status = "ambiguous_owner";
                break;
            case source::ServiceIssue::UnmatchedDevice:
                status = "unmatched_device_port";
                break;
            case source::ServiceIssue::NonServiceDevice:
                status = "non_service_device";
                break;
            }
        }
        ++totals[status];
        auto & summary = summaries[observed.work_observations[id].description];
        ++summary.statuses[status];
        if (!service.available()) continue;

        graph::Time duration_ns = 0;
        for (const auto node : service.nodes) duration_ns += source.graph().graph.nodes()[node].duration_ns;
        summary.nodes += service.nodes.size();
        summary.minimum_ns = std::min(summary.minimum_ns, duration_ns);
        summary.maximum_ns = std::max(summary.maximum_ns, duration_ns);
        if (!summary.example) summary.example = id;
    }
    auto descriptions = Json::array();
    for (std::size_t id = 0; id < summaries.size(); ++id) {
        const auto & summary = summaries[id];
        Json example = nullptr;
        if (summary.example) {
            const auto & call = observed.work_observations[*summary.example];
            const auto & service = source.device_service(*summary.example);
            const auto & origin = source.graph().node_origins[service.nodes.front()];
            const auto & device = observed.spans[origin.span];
            example = {
                {     "work_observation",                                                         *summary.example },
                {                "probe",     { { "file", call.record.file }, { "record", call.record.position } } },
                { "first_device_service", { { "file", device.record.file }, { "record", device.record.position } } }
            };
        }
        descriptions.push_back({
            {      "work_description",                                                         id },
            {              "statuses",                                           summary.statuses },
            { "bound_exclusive_nodes",                                              summary.nodes },
            {    "minimum_service_ns", summary.example ? Json(summary.minimum_ns) : Json(nullptr) },
            {    "maximum_service_ns", summary.example ? Json(summary.maximum_ns) : Json(nullptr) },
            {               "example",                                         std::move(example) }
        });
    }
    return {
        {      "statuses",totals                          },
        {  "descriptions", std::move(descriptions) },
        { "qualification",
         "Observed native submission ownership only; no probe wall costs, ambiguous parent partitioning, capture-completeness or cross-deployment transfer "
         "claim."                                 }
    };
}

Json source_compute_alignment(const source::SourceContext & source) {
    const auto & captured = source.observations();
    std::size_t unavailable = 0, missing_bindings = 0, compared = 0, mismatched = 0, recipes = 0, dependencies = 0;
    auto examples = Json::array();
    for (source::Index frame_id = 0; frame_id < captured.compute_frames.size(); ++frame_id) {
        const auto & frame = captured.compute_frames[frame_id];
        if (!frame.query_tokens || frame.query_tokens->size() != 1 || frame.sequence_tokens.size() != 1 || !frame.page_tokens) {
            ++unavailable;
            continue;
        }
        const auto query = frame.query_tokens->front();
        const auto plan = planning::project_compute(source, frame_id, { query, query, frame.sequence_tokens.front(), 1, *frame.page_tokens });
        missing_bindings += plan.missing_row_bindings.size();
        dependencies += plan.dependencies.size();
        std::size_t at = 0;
        for (const auto operation : source.frame_operations(frame_id)) {
            ++recipes;
            const auto & call = captured.work_observations[operation];
            const auto & original = captured.work_descriptions[call.description].observed;
            const auto begin = at;
            while (at < plan.operations.size() && plan.operations[at].source_operation == operation) ++at;
            const auto actual_count = at - begin;
            if (original.empty() && actual_count == 1 && !plan.operations[begin].work) continue;
            bool different = false;
            for (std::size_t i = 0; i < std::max(actual_count, original.size()); ++i) {
                ++compared;
                if (i < actual_count && i < original.size() && plan.operations[begin + i].work && plan.work[*plan.operations[begin + i].work] == original[i])
                    continue;

                ++mismatched;
                different = true;
            }
            constexpr std::size_t example_limit = 5;
            if (different && examples.size() < example_limit)
                examples.push_back({
                    {            "frame",  frame_id },
                    { "source_operation", operation }
                });
        }
    }
    return {
        {       "compared_operations",            compared },
        {          "compared_recipes",             recipes },
        { "intra_recipe_dependencies",        dependencies },
        {     "mismatched_operations",          mismatched },
        {      "missing_row_bindings",    missing_bindings },
        {        "unavailable_frames",         unavailable },
        {                  "examples", std::move(examples) }
    };
}

Json timing_alignment(const source::Observations & observations, const source::SourceGraph & source, const simulation::ExecutionResult & replay,
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

Json replay_summary(const source::Observations & observations, const source::SourceGraph & built_source, const simulation::ExecutionResult & replay,
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

Json token_length(const std::optional<source::TokenObservation> & observed, const planning::TokenPaths & paths) {
    return observed ? Json(paths.length(observed->path)) : Json(nullptr);
}

Json request_token_summary(const source::Observations & captured) {
    auto result = Json::array();
    const source::RequestTokens missing;
    for (const auto & request : captured.requests) {
        const auto found = captured.request_tokens.find(request.identity);
        const auto & tokens = found == captured.request_tokens.end() ? missing : found->second;
        result.push_back({
            { "request", request.identity },
            { "formal", request.formal },
            { "prompt_tokens", token_length(tokens.prompt, captured.token_paths) },
            { "lookup_input_tokens", token_length(tokens.lookup_input, captured.token_paths) },
            { "committed_tokens", token_length(tokens.committed, captured.token_paths) }
        });
    }
    return result;
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
    for (const auto & description : captured.work_descriptions) {
        auto operations = Json::array();
        for (const auto & work : description.observed) operations.push_back(work_json(work));
        work_descriptions.push_back({
            { "operations", std::move(operations) }
        });
    }
    auto frames = Json::array();
    for (const auto & frame : captured.compute_frames) {
        auto requests = Json::array();
        for (const auto request : frame.requests) requests.push_back(captured.names[request]);
        frames.push_back({
            {           "phase", frame.phase == planning::Phase::Prefill ? "prefill" : "decode" },
            {        "requests",                                            std::move(requests) },
            { "sequence_tokens",                                          frame.sequence_tokens },
            {    "query_tokens", frame.query_tokens ? Json(*frame.query_tokens) : Json(nullptr) },
            {     "page_tokens",   frame.page_tokens ? Json(*frame.page_tokens) : Json(nullptr) }
        });
    }
    auto groups = Json::array();
    for (const auto & group : captured.collective_groups)
        groups.push_back({
            { "identity", group.identity },
            {  "members",  group.members }
        });
    std::map<std::string, std::size_t> collective_calls;
    for (const auto & call : captured.collectives) ++collective_calls[call.work.kind.operation];
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
        {         "work_description_scope",                                                                                         "observed_compute_recipe_subset" },
        {                 "compute_frames",                                                                                                        std::move(frames) },
        {              "collective_groups",                                                                                                        std::move(groups) },
        {        "collective_observations",                                                                                              std::move(collective_calls) },
        {      "source_compute_projection",                                                                                        source_compute_alignment(context) },
        {         "device_service_binding",                                                                                          device_service_summary(context) },
        {             "cpu_work_intervals",                                                                                            cpu_interval_summary(context) },
        {               "source_cpu_costs",                                                                                           source_cpu_cost_report(source) },
        {                 "request_tokens",                                                                                          request_token_summary(captured) },
        {         "logical_token_prefixes",                                                                                              captured.token_paths.size() },
        {        "work_observation_counts",                                                                                                   std::move(work_counts) },
        {              "work_observations",                                                                                        captured.work_observations.size() },
        {          "work_observation_unit",                                                                                              "native_compute_probe_call" },
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
