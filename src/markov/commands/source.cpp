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
#include <tuple>
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
    struct Preparation {
        Summary intervals;
        std::size_t rules = 0, resources = 0, queues = 0, native_sequences = 0, native_calls = 0;
        std::map<std::string, std::size_t> native_issues;
        std::map<std::tuple<std::size_t, std::size_t, std::size_t>, std::size_t> contexts;
    };
    std::map<std::string, Preparation> preparations;
    std::size_t unsupported_frames = 0;
    for (source::Index frame = 0; frame < observed.compute_frames.size(); ++frame) {
        unsupported_frames += !source.frame_cpu_order_supported(frame);
        const auto & regions = observed.compute_frames[frame].preparations;
        const auto intervals = source.preparation_intervals(frame);
        const auto bindings = source.preparation_bindings(frame);
        for (std::size_t i = 0; i < regions.size(); ++i) {
            auto & item = preparations[regions[i].role];
            item.intervals.include(intervals[i], regions[i].region.end_ns - regions[i].region.begin_ns);
            item.rules += regions[i].definition != nullptr;
            const auto resources = bindings[i].unique_resources();
            item.resources += resources.has_value();
            item.queues += bindings[i].workers.size() == 1;
            const auto & native = source.preparation_native_calls(frame, i);
            if (native.issue) ++item.native_issues[*native.issue];
            else {
                ++item.native_sequences;
                item.native_calls += native.calls.size();
            }
            if (resources) {
                const auto & r = *resources;
                ++item.contexts[{ r.caller, r.worker, r.device }];
            }
        }
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
    auto preparation_report = Json::object();
    for (const auto & [role, preparation] : preparations) {
        auto item = describe(preparation.intervals);
        item["rules"] = preparation.rules;
        item["native_resource_bindings"] = preparation.resources;
        item["native_queue_bindings"] = preparation.queues;
        item["matched_native_sequences"] = preparation.native_sequences;
        item["matched_native_calls"] = preparation.native_calls;
        item["native_sequence_issues"] = preparation.native_issues;
        auto contexts = Json::array();
        for (const auto & [context, count] : preparation.contexts) {
            const auto [caller, worker, device] = context;
            contexts.push_back({
                { "caller", observed.resources[caller].identity },
                { "worker", observed.resources[worker].identity },
                { "device", observed.resources[device].identity },
                {  "calls",                               count }
            });
        }
        item["execution_contexts"] = std::move(contexts);
        preparation_report[role] = std::move(item);
    }
    std::size_t unmodeled_gap_work = 0;
    std::map<std::string, std::size_t> gap_followers;
    for (source::Index frame = 0; frame < observed.compute_frames.size(); ++frame) {
        for (const auto & entry : source.frame_calls(frame)) {
            if (!entry.gap_before || !source.cpu_gap_has_unmodeled_work(*entry.gap_before)) continue;
            ++unmodeled_gap_work;
            if (entry.kind == source::FrameCallKind::Preparation) ++gap_followers[observed.compute_frames[frame].preparations[entry.index].role];
            else if (entry.kind == source::FrameCallKind::Collective) ++gap_followers[observed.collectives[entry.index].work.kind.operation];
            else {
                const auto & description = observed.work_descriptions[observed.work_observations[entry.index].description];
                ++gap_followers[description.observed.empty() ? "view_only" : description.observed.front().kind.operation];
            }
        }
    }

    return {
        {                                 "compute_calls",std::move(descriptions)                                                          },
        {                                  "preparations", std::move(preparation_report) },
        {                        "inter_call_cpu_regions",      source.cpu_gaps().size() },
        {             "frames_with_unsupported_cpu_order",            unsupported_frames },
        { "inter_call_regions_with_unmodeled_submissions",            unmodeled_gap_work },
        {                "unmodeled_submission_followers",                 gap_followers },
        {                                 "qualification",
         "Source caller intervals only: exclusive observed service, waits, gaps, and uncovered time are kept separate. "
         "Profiler correction is not applied; these are neither normal CPU costs nor a complete request partition. Device/worker work is not charged "
         "here."                                                                        }
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
    std::map<std::pair<source::ResourceKind, graph::NodeKind>, Alignment> groups;
    for (std::size_t id = 0; id < source.graph.nodes().size(); ++id) {
        const auto & node = source.graph.nodes()[id];
        auto & group = groups[{ observations.resources[node.resource].kind, node.kind }];
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
    const std::map<graph::NodeKind, std::string> node_kinds{
        {          graph::NodeKind::Service,           "service" },
        {     graph::NodeKind::ObservedWait,     "observed_wait" },
        { graph::NodeKind::ProfilerOverhead, "profiler_overhead" },
        {      graph::NodeKind::ResidualGap,      "residual_gap" },
        {         graph::NodeKind::Boundary,          "boundary" },
        {            graph::NodeKind::Delay,             "delay" }
    };
    for (const auto & [identity, group] : groups) {
        const auto [kind, node_kind] = identity;
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
            if (!example.is_null()) {
                const auto & operation = source.graph.nodes()[group.largest_node];
                example["resource"] = observations.resources[operation.resource].identity;
                example["observed_start_ns"] = node.begin_ns;
                example["replayed_start_ns"] = replay.starts_ns[group.largest_node] + origin;
                example["duration_ns"] = operation.duration_ns;
            }
        }
        const auto label = kind == source::ResourceKind::CpuThread ? "cpu_thread" : kind == source::ResourceKind::DeviceStream ? "device_stream" : "client";
        result.push_back({
            {                     "resource",                                                                                  label },
            {                    "cost_role", node_kind == graph::NodeKind::ProfilerOverhead ? "profiler_overhead" : "observed_work" },
            {                    "node_kind",                                                               node_kinds.at(node_kind) },
            {                        "nodes",                                                                            group.nodes },
            {                "earlier_nodes",                                                                          group.earlier },
            {                  "later_nodes",                                                                            group.later },
            {  "max_absolute_start_delta_ns",                                                            group.max_absolute_delta_ns },
            {     "largest_interval_example",                                                                     std::move(example) },
            { "mean_absolute_start_delta_ns",                                                 group.absolute_delta_sum / group.nodes }
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
    const auto region_json = [](const source::ObservedRegion & region) -> Json {
        return {
            { "resource",        region.resource },
            { "begin_ns",        region.begin_ns },
            {   "end_ns",          region.end_ns },
            {     "file",     region.record.file },
            {   "record", region.record.position }
        };
    };
    auto results = Json::array();
    for (const auto & result : captured.batch_results) {
        Json value{
            {       "frame",            result.frame },
            {         "run", region_json(result.run) },
            {     "overlap",          result.overlap },
            {        "copy",                 nullptr },
            {   "processed",                 nullptr },
            { "final_sends",           Json::array() }
        };
        if (result.copy) {
            const auto & copy = *result.copy;
            value["copy"] = {
                {             "region", region_json(copy.region) },
                {        "token_shape",         copy.token_shape },
                {        "token_dtype",         copy.token_dtype },
                {       "token_device",        copy.token_device },
                {        "token_bytes",         copy.token_bytes },
                { "additional_outputs",  copy.additional_outputs }
            };
        }
        if (result.processed) {
            const auto & processed = *result.processed;
            value["processed"] = {
                {               "region",  region_json(processed.region) },
                { "output_tokens_before", processed.output_tokens_before },
                {  "output_tokens_after",  processed.output_tokens_after },
                {      "finished_before",      processed.finished_before },
                {       "finished_after",       processed.finished_after }
            };
        }
        for (const auto & send : result.final_sends) {
            auto requests = Json::array();
            for (const auto request : send.requests) requests.push_back(captured.names[request]);
            value["final_sends"].push_back({
                {   "region", region_json(send.region) },
                { "requests",      std::move(requests) }
            });
        }
        results.push_back(std::move(value));
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
        {                  "configuration",                                manifest.configuration_path ? Json(manifest.configuration_path->string()) : Json(nullptr) },
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
        {                  "batch_results",                                                                                                       std::move(results) },
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
