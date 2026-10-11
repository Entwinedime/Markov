#include "markov/trace_graph/source_dag/replay.hpp"

#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <utility>
#include <vector>

namespace markov::trace_graph::source_dag {

namespace {

// A blocking host return cannot precede the device completion it waits for.
// Audit preserved observations, not simulated times or normalized wait costs.
// A violation cannot distinguish clock conversion from an incorrect edge.
nlohmann::json synchronization_clock_report(const core::DagGraph & graph) {
    using Json = nlohmann::json;
    struct RankAudit {
        size_t checked = 0, unavailable = 0, violations = 0;
        uint64_t max_lateness_ns = 0;
        Json example = nullptr;
    };
    std::map<int, RankAudit> ranks;
    for (const auto & edge : graph.edges()) {
        if (!edge.active || edge.kind != core::DagEdgeKind::Sync) continue;
        const auto & producer = graph.node(edge.src);
        const auto & consumer = graph.node(edge.dst);
        if (!producer.active || !consumer.active || producer.is_cpu || !consumer.is_cpu) continue;
        const auto & wait = graph.event_for_node(edge.dst);
        if (!core::synchronizes_device_frontier(wait) && !core::synchronizes_recorded_event(wait)) continue;

        auto & rank = ranks[consumer.gpu_id];
        if (producer.kind != core::DagNodeKind::TraceEvent || consumer.kind != core::DagNodeKind::TraceEvent) {
            ++rank.unavailable;
            continue;
        }
        ++rank.checked;
        const auto & device = graph.event_for_node(edge.src);
        const auto completed = device.end_ns(), returned = wait.end_ns();
        if (completed <= returned) continue;

        ++rank.violations;
        const auto lateness = completed - returned;
        if (lateness <= rank.max_lateness_ns) continue;
        rank.max_lateness_ns = lateness;
        rank.example = {
            {         "device_node",    edge.src },
            {           "host_node",    edge.dst },
            {    "device_operation", device.name },
            {      "host_operation",   wait.name },
            { "device_completed_ns",   completed },
            {    "host_returned_ns",    returned }
        };
    }
    auto inputs = Json::array();
    for (const auto & [input, rank] : ranks) {
        inputs.push_back({
            {                "logical_input",                                                     input },
            {         "checked_dependencies",                                              rank.checked },
            {     "unavailable_observations",                                          rank.unavailable },
            { "completion_after_host_return",                                           rank.violations },
            {              "max_lateness_ns", rank.checked ? Json(rank.max_lateness_ns) : Json(nullptr) },
            {            "largest_violation",                                              rank.example }
        });
    }
    return {
        {      "scope","observed device completion versus blocking host return on existing synchronization edges"                       },
        {     "inputs",                                                                          std::move(inputs) },
        { "limitation",
         "Checks existing edges only. Violations require clock/association investigation; this report neither shifts clocks nor proves missing "
         "dependencies."                                                                                          }
    };
}

} // namespace

nlohmann::json input_selection_report(std::span<const io::ManifestTraceInput> inputs) {
    auto rows = nlohmann::json::array();
    for (size_t i = 0; i < inputs.size(); ++i) {
        const auto & input = inputs[i];
        const auto retained = input.duration_record_count - input.window_removed_record_count;
        const auto tails = std::ranges::count_if(input.events, [](const auto & event) { return event.arg("formal_window_context") == "causal_tail"; });
        rows.push_back({
            {                "logical_input",                                                       i },
            {               "joined_records",                               input.joined_record_count },
            { "non_duration_records_removed", input.joined_record_count - input.duration_record_count },
            {             "duration_records",                             input.duration_record_count },
            {       "window_records_removed",                       input.window_removed_record_count },
            {    "retained_duration_records",                                                retained },
            {  "synthetic_window_boundaries",                             input.window_boundary_count },
            { "retained_causal_tail_records",                                                   tails },
            {     "argument_context_records",                             input.context_events.size() },
            {     "semantic_prelude_records",                     input.prelude_context_events.size() },
            {        "semantic_tail_records",                        input.tail_context_events.size() }
        });
    }
    return {
        {      "scope","parsed and joined manifest channels before DAG normalization"                       },
        {     "inputs",                                                std::move(rows) },
        { "limitation",
         "Parsing and channel joining precede these counts. Context tables may overlap selected events; neither omitted raw records nor semantic dependency "
         "coverage is proven."                                                        }
    };
}

nlohmann::json replay_report(const core::DagGraph & source, const core::ClientRequestChain & chain, std::span<const core::ClientRequestTiming> requests,
                             size_t execution_event_count, std::string_view manifest) {
    using Json = nlohmann::json;
    Json report{
        {                         "stage",                                      "source_dag" },
        {                    "cost_basis", "source_observed_with_builder_wait_normalization" },
        {               "source_manifest",                                          manifest },
        {           "client_chain_status",                                      chain.status },
        { "dependency_correctness_proven",                                             false },
    };
    const auto topology = core::validate_active_dag(source);
    Json issues = Json::array();
    for (const auto & issue : topology.issues)
        issues.push_back({
            {    "code",         issue.code },
            { "message",      issue.message },
            {   "nodes",     issue.node_ids },
            {   "edges", issue.edge_indices }
        });

    std::vector<bool> represented_events(execution_event_count);
    size_t rewritten_trace_durations = 0;
    for (const auto & node : source.nodes()) {
        if (node.active && node.event_index < execution_event_count) represented_events[node.event_index] = true;
        if (node.active && node.kind == core::DagNodeKind::TraceEvent && node.duration != source.event_for_node(node.id).dur) ++rewritten_trace_durations;
    }
    const auto represented_count = std::ranges::count(represented_events, true);
    const auto stats = source.summary_stats();
    report.update({
        {     "timing_basis",
         { { "cpu_overhead_correction_applied", false },
         { "independent_cost_model_applied", false },
         { "target_transform_applied", false },
         { "nodes_with_rewritten_trace_duration", rewritten_trace_durations },
         { "limitation",
         "Builder synchronization and CPU queue normalization replace observed waits with dependencies; replay is not an untouched sum of raw trace "
         "durations." } }                                                                                               },
        {   "event_coverage",
         { { "scope", "normalized executable events selected by manifest channels and formal window" },
         { "selected_joined_records", source.parsed_record_count() },
         { "normalized_executable_events", execution_event_count },
         { "represented_normalized_events", represented_count },
         { "unrepresented_normalized_events", execution_event_count - static_cast<size_t>(represented_count) },
         { "fact_observations", source.hicache_fact_events().size() },
         { "runtime_observations", source.runtime_observations().size() },
         { "raw_record_coverage_proven", false },
         { "limitation", "Normalization removes duplicates and nested envelopes; counts are not raw trace coverage." } } },
        { "dependency_audit",
         { { "scope", "active graph endpoints, duplicate mutation edges and cycles" },
         { "topology_valid", topology.ok() },
         { "active_nodes", topology.active_node_count },
         { "active_edges", topology.active_edge_count },
         { "edge_kind_counts", stats.edge_counts_by_kind },
         { "issues", issues },
         { "observed_clock_order", synchronization_clock_report(source) },
         { "limitation", "Acyclicity does not prove that every required semantic dependency is present." } }            },
    });
    if (chain.status != "connected") {
        report["status"] = "unavailable_http_window";
        return report;
    }

    // Queue materialization and timestamps belong to this private replay.
    auto graph = source;
    (void)simulation::run_topological_simulation(graph);
    uint64_t completion = 0;
    Json replayed_requests = Json::array();
    double request_observed_sum_us = 0.0;
    double request_absolute_error_sum_us = 0.0;
    double request_max_ape = 0.0;
    // connect_client_requests preserves the validated serial input order.
    // Compare each request's own latency, excluding the inter-request pause.
    for (size_t i = 0; i < chain.requests.size(); ++i) {
        const auto & request = chain.requests[i];
        const auto & observed = requests[i];
        const auto start = graph.node(request.start).completion_time;
        const auto end = graph.node(request.completion).completion_time;
        completion = std::max(completion, end);
        const auto observed_latency = observed.end_us - observed.start_us;
        const auto replayed_latency = end - start;
        const auto error = static_cast<double>(replayed_latency) - static_cast<double>(observed_latency);
        const auto ape = std::abs(error) / static_cast<double>(observed_latency);
        request_observed_sum_us += static_cast<double>(observed_latency);
        request_absolute_error_sum_us += std::abs(error);
        request_max_ape = std::max(request_max_ape, ape);

        replayed_requests.push_back({
            {                "request_id", request.request_id },
            {                  "start_us",              start },
            {             "completion_us",                end },
            {       "observed_latency_us",   observed_latency },
            {       "replayed_latency_us",   replayed_latency },
            {           "signed_error_us",              error },
            { "absolute_percentage_error",                ape }
        });
    }
    const auto observed_us = requests.back().end_us - requests.front().start_us;
    const auto replayed_us = completion - graph.node(chain.requests.front().start).completion_time;
    const auto signed_error = static_cast<double>(replayed_us) - static_cast<double>(observed_us);
    report.update({
        {                    "status",                                                  "reported"                                      },
        {                     "scope",                                                                 "same_profile_http_formal_window" },
        {           "observed_e2e_us",                                                                                       observed_us },
        {           "replayed_e2e_us",                                                                                       replayed_us },
        {           "signed_error_us",                                                                                      signed_error },
        {         "absolute_error_us",                                                                            std::abs(signed_error) },
        { "absolute_percentage_error", observed_us > 0 ? Json(std::abs(signed_error) / static_cast<double>(observed_us)) : Json(nullptr) },
        {  "request_latency_accuracy",
         { { "scope", "same-profile per-request HTTP latency, excluding inter-request pauses" },
         { "wape", request_absolute_error_sum_us / request_observed_sum_us },
         { "max_ape", request_max_ape },
         { "request_count", chain.requests.size() } }                                                                                   },
        {             "request_count",                                                                             chain.requests.size() },
        {          "graph_node_count",                                                                             source.nodes().size() },
        {                  "requests",                                                                                 replayed_requests },
    });
    return report;
}

} // namespace markov::trace_graph::source_dag
