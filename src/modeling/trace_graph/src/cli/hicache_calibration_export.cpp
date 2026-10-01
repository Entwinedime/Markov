// Offline extraction from an explicitly selected independent calibration only.
#include "markov/trace_graph/cli/input_graph.hpp"
#include "markov/trace_graph/io/cpu_service_input.hpp"
#include "markov/trace_graph/io/trace_manifest_input.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/model/prefetch_wait_calibration.hpp"
#include "markov/trace_graph/modules/hicache/patch/layer_io.hpp"
#include "markov/trace_graph/modules/hicache/patch/layer_wait_insertion.hpp"
#include "markov/trace_graph/modules/hicache/runtime/control_calibration.hpp"
#include "markov/trace_graph/modules/hicache/runtime/load_execution.hpp"
#include "markov/trace_graph/modules/hicache/runtime/prefetch_queries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include <algorithm>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
using namespace markov::trace_graph;
namespace hc = modules::hicache;

namespace {

// Local node ids are array offsets. CPU and device service share the same unit;
// residual CPU gaps and worker dispatch delays remain separate observations.
nlohmann::json expansion_observation(const hc::runtime::HiCacheHostExpansion & plan) {
    auto nodes = nlohmann::json::array();
    for (size_t i = 0; i < plan.nodes.size(); ++i) {
        const auto & node = plan.nodes[i];
        nlohmann::json item{
            {          "id",                             i },
            {        "name",                node.work.name },
            {         "cpu",              node.work.is_cpu },
            {        "lane",            node.work.lane_key },
            {  "service_us",            node.work.duration },
            { "residual_us",       node.work.cpu_gap_after },
            {      "worker", node.queue_member.has_value() }
        };
        if (node.submission) item["submission"] = *node.submission;
        if (node.work.cpu_task_ready_delay_us) item["dispatch_us"] = *node.work.cpu_task_ready_delay_us;
        nodes.push_back(std::move(item));
    }

    auto edges = nlohmann::json::array();
    for (const auto & edge : plan.edges) edges.push_back({ edge.from, edge.to, static_cast<int>(edge.kind) });

    return {
        {       "nodes",        std::move(nodes) },
        {       "edges",        std::move(edges) },
        { "event_waits", plan.event_waits.size() }
    };
}

int export_operation(core::DagGraph graph, const std::string & manifest, uint64_t begin_us, uint64_t end_us, const std::string & output_path,
                     const std::string & operation, const std::string & cpu_service) {
    const bool corrected = cpu_service != "-";
    const auto publish = [&](nlohmann::json document) {
        document.update({
            {  "source_manifest",                                      manifest },
            { "cpu_service_file",                  corrected ? cpu_service : "" },
            {       "cost_basis", corrected ? "paired_cpu_service" : "profiled" }
        });
        std::ofstream output(output_path);
        output << document.dump(2) << '\n';
        if (!output) throw std::runtime_error("Failed to persist operation calibration");
    };

    const hc::patch::HiCacheSourceDagIndex source(graph);
    if (operation == "prefetch-query") {
        const auto rounds = hc::observe_cpu_collectives(source);
        const auto observed = hc::runtime::observe_prefetch_query_template(graph, rounds, hc::observe_prefetch_workers(source, rounds), begin_us, end_us);
        if (!observed.issue.empty()) throw std::runtime_error(observed.issue);

        auto document = hc::model::encode_prefetch_query_calibration({ manifest, observed.timing });
        document["limitations"] = "First complete independent query; CPU correction only where paired measurements exist. Residual and communication timing "
                                  "retain source conditions.";
        publish(std::move(document));
        return 0;
    }
    if (operation == "write-host" || operation == "release-host") {
        if (!corrected) throw std::runtime_error("Shared write/release costs require paired CPU correction");
        const bool release_only = operation == "release-host";
        auto document = hc::runtime::export_write_calibration(graph, begin_us, end_us, release_only);
        document.update({
            {        "role",                                                            "fixed_calibration"              },
            { "limitations",
             release_only ? "Observed ordinary-release program with paired CPU correction; residuals and device work retained. Nearest-size reuse is "
             "unvalidated extrapolation."
             : "CPU correction only where measured; residual gaps retained. Target geometry still requires FAST2D proof." }
        });
        publish(std::move(document));
        return 0;
    }
    if (operation.starts_with("prefetch-wait:")) {
        const auto policy = operation.substr(operation.find(':') + 1);
        if (policy != "best_effort" && policy != "timeout" && policy != "wait_complete") return 2;
        const auto rounds = hc::observe_cpu_collectives(source);
        const auto workers = hc::observe_prefetch_workers(source, rounds);
        const auto grouped = hc::observe_prefetch_requests(source, rounds, policy);
        std::vector<std::pair<uint64_t, std::string>> requests;
        for (const auto & [request, ranks] : grouped) {
            auto at = ranks.begin()->second.front().cpu.front().cpu.interval_start_us;
            for (const auto & [rank, calls] : ranks) at = std::min(at, calls.front().cpu.front().cpu.interval_start_us);
            requests.emplace_back(at, request);
        }
        std::ranges::sort(requests);
        // Local returns precede policy dispatch and can occur in a different
        // request from the active branch. Retain their own measured CPU costs.
        std::map<int, uint64_t> local_returns;
        for (const auto & [at, request] : requests)
            for (const auto & [rank, calls] : grouped.at(request)) {
                if (local_returns.contains(rank)) continue;
                for (const auto & call : calls)
                    if (call.local_return) {
                        local_returns.emplace(rank, *hc::model::observe_prefetch_check_cpu(call, &source).no_operation_return);
                        break;
                    }
            }

        for (const auto & [at, request] : requests) {
            const auto & ranks = grouped.at(request);
            auto timing = policy == "best_effort" ? hc::model::observe_prefetch_stop_timing(source, rounds, ranks)
                                                  : hc::model::observe_prefetch_wait_timing(source, rounds, workers, ranks);
            if (!timing) continue;
            for (auto & [rank, cpu] : timing->check.cpu)
                if (!cpu.no_operation_return && local_returns.contains(rank)) cpu.no_operation_return = local_returns.at(rank);

            auto document = hc::model::encode_prefetch_wait_calibration({ manifest, request, policy, *timing });
            (void)hc::model::decode_prefetch_wait_calibration(document);
            document["estimator"] = "First complete active request and first local return per rank in source time order; target state determines retries.";
            document["limitations"] = "Local CPU correction only where paired measurements exist; residual scheduler gaps, collective dispatch and worker "
                                      "timing retain source conditions.";
            publish(std::move(document));
            return 0;
        }
        throw std::runtime_error("Independent capture has no complete active prefetch branch");
    }
    if (operation == "audit-layer-wait") {
        const auto queues = simulation::detail::discover_cpu_task_queues(graph);
        auto rows = nlohmann::json::array();
        for (const auto & call : hc::observe_hicache_layer_waits(source).calls) {
            if (!call.enabled) continue;
            nlohmann::json row{
                { "phase", call.phase },
                { "layer", call.layer }
            };
            try {
                if (!call.submission || !call.worker || !call.device_wait || !call.issue.empty())
                    throw std::runtime_error("Independent layer wait lacks complete submission evidence");
                hc::runtime::HiCacheHostTemplate host{ .main = call.cpu, .worker_nodes = { *call.worker } };
                const auto plan = hc::runtime::prepare_host_expansion(source, queues, host);
                if (plan.streams.size() != 1 || plan.event_waits.size() != 1 || !plan.waits.empty())
                    throw std::runtime_error("Independent layer wait has unsupported dependencies");
                const auto submission = plan.source_nodes.at(*call.submission);
                uint64_t before = 0, before_gap = 0, after = 0, after_gap = 0, worker = 0, dispatch = 0;
                size_t jobs = 0;
                for (size_t i = 0; i < plan.nodes.size(); ++i) {
                    const auto & node = plan.nodes[i];
                    if (!node.work.is_cpu) continue;
                    if (node.submission) {
                        ++jobs;
                        worker += node.work.duration;
                        dispatch += node.work.cpu_task_ready_delay_us.value_or(0);
                    }
                    else if (i <= submission) {
                        before += node.work.duration;
                        before_gap += node.work.cpu_gap_after;
                    }
                    else {
                        after += node.work.duration;
                        after_gap += node.work.cpu_gap_after;
                    }
                }
                if (jobs != 1) throw std::runtime_error("Layer wait must have one background task");
                row.update({
                    {            "status",                             "observed" },
                    {              "rank",    graph.node(*call.submission).gpu_id },
                    {           "main_us",                                 before },
                    {  "main_residual_us",                             before_gap },
                    {          "after_us",                                  after },
                    { "after_residual_us",                              after_gap },
                    {         "worker_us",                                 worker },
                    {       "dispatch_us",                               dispatch },
                    {         "device_us", graph.node(*call.device_wait).duration }
                });
            }
            catch (const std::exception & error) {
                row.update({
                    { "status", "incomplete" },
                    { "reason", error.what() }
                });
            }
            rows.push_back(std::move(row));
        }
        publish({
            { "role", "independent_layer_wait_audit" },
            { "rows",                std::move(rows) }
        });
        return 0;
    }
    if (operation == "audit-load-submission") {
        const auto ledger = hc::patch::build_hicache_io_operation_ledger(source);
        std::vector<core::TraceEvent> points;
        for (const auto & record : ledger.records)
            if (record.kind == hc::patch::HiCacheIoOperationKind::Load && !record.device_transfer_node_ids.empty())
                for (const auto at : { record.source_start_us, record.source_end_us }) {
                    core::TraceEvent point;
                    point.name = "load submission cost boundary";
                    point.pid = record.pid;
                    point.tid = record.tid;
                    point.ts = at;
                    points.push_back(std::move(point));
                }
        (void)hc::bind_hicache_control_points(graph, points);
        const hc::patch::HiCacheSourceDagIndex bounded(graph);
        const auto waits = hc::observe_hicache_layer_waits(bounded);
        const auto queues = simulation::detail::discover_cpu_task_queues(graph);
        auto rows = nlohmann::json::array();
        for (const auto & record : ledger.records) {
            if (record.kind != hc::patch::HiCacheIoOperationKind::Load || record.device_transfer_node_ids.empty()) continue;
            nlohmann::json row{
                {      "pid",             record.pid },
                { "start_us", record.source_start_us }
            };
            try {
                hc::patch::HiCacheRewriteDecision decision;
                decision.request_id = record.request_id;
                decision.source_fact_node_id = record.timing_fact_node_id;
                decision.source_readiness_topology_reused = record.source_readiness_topology_ready;
                decision.owned_duration_nodes = record.device_transfer_node_ids;
                const auto layers = hc::patch::observe_hicache_layer_transfers(bounded, waits, decision);
                if (layers.empty()) throw std::runtime_error("No complete independent load layers");
                const auto host = hc::runtime::observe_host_template(bounded, record.pid, record.tid, record.source_start_us, record.source_end_us);
                const auto plan = hc::runtime::prepare_write_expansion(bounded, queues, record, host, layers.back().source_record);
                const auto costs = hc::runtime::observe_load_submission_cost(plan, layers.size());
                if (!costs) throw std::runtime_error("Load submission cannot be partitioned into supported Ascend operations");
                using Cost = hc::runtime::LoadSubmissionCost;
                for (const auto & [name, member] : {
                         std::pair{        "before_sync",        &Cost::before_sync },
                         {       "start_record",       &Cost::start_record },
                         {         "wait_event",         &Cost::wait_event },
                         {         "first_copy",         &Cost::first_copy },
                         {               "copy",               &Cost::copy },
                         { "first_layer_record", &Cost::first_layer_record },
                         {       "layer_record",       &Cost::layer_record },
                         {               "tail",               &Cost::tail }
                }) {
                    const auto & cost = *costs.*member;
                    row["costs"][name] = {
                        {          "main_us",          cost.main_us },
                        { "main_residual_us", cost.main_residual_us },
                        {        "worker_us",        cost.worker_us },
                        {      "dispatch_us",      cost.dispatch_us },
                        {        "device_us",        cost.device_us }
                    };
                }
                row.update(expansion_observation(plan));
                for (const auto & [node, bytes] : plan.payload) row["nodes"][node]["payload_bytes"] = bytes;

                auto streams = nlohmann::json::array(), barriers = nlohmann::json::array();
                for (const auto & stream : plan.streams)
                    streams.push_back({
                        {  "lane", graph.node_lane_key(stream.source_node) },
                        { "first",                            stream.first },
                        {  "last",                             stream.last }
                    });
                for (const auto & wait : plan.waits)
                    barriers.push_back({
                        {     "lane", graph.node_lane_key(wait.source_node) },
                        { "consumer",                         wait.consumer }
                    });
                std::vector<size_t> records;
                for (const auto & layer : layers) records.push_back(plan.source_nodes.at(layer.source_record));
                row.update({
                    {      "status",                                          "observed" },
                    {        "rank", graph.node(host.main.owned_node_ids.front()).gpu_id },
                    {     "streams",                                             streams },
                    {       "waits",                                            barriers },
                    {     "records",                                             records },
                    { "host_return",                                    plan.host_return },
                    {       "bytes",                                  plan.payload_bytes }
                });
            }
            catch (const std::exception & error) {
                row.update({
                    { "status", "incomplete" },
                    { "reason", error.what() }
                });
            }
            rows.push_back(std::move(row));
        }
        publish({
            { "role", "independent_load_submission_audit" },
            { "rows",                     std::move(rows) }
        });
        return 0;
    }
    if (operation == "audit-load") {
        std::vector<core::TraceEvent> points;
        for (const auto & fact : source.fact_nodes())
            if (fact.fact_role == "loadback_decision_observed" && fact.phase == "end" && fact.effective_token_count)
                for (const auto at : { fact.timestamp_us, fact.timestamp_us + fact.duration_us }) {
                    core::TraceEvent point;
                    point.name = "independent load cost boundary";
                    point.pid = fact.pid;
                    point.tid = fact.tid;
                    point.ts = at;
                    points.push_back(std::move(point));
                }
        const auto boundaries = hc::bind_hicache_control_points(graph, points);
        const hc::patch::HiCacheSourceDagIndex bounded(graph);
        const auto queues = simulation::detail::discover_cpu_task_queues(graph);
        nlohmann::json rows = nlohmann::json::array();
        for (const auto & fact : bounded.fact_nodes()) {
            if (fact.fact_role != "loadback_decision_observed" || fact.phase != "end" || !fact.effective_token_count) continue;
            nlohmann::json row{
                {      "tokens", fact.effective_token_count },
                {         "pid",                   fact.pid },
                {    "begin_us",          fact.timestamp_us },
                { "duration_us",           fact.duration_us }
            };
            try {
                const auto host = hc::runtime::observe_host_template(bounded, fact.pid, fact.tid, fact.timestamp_us, fact.timestamp_us + fact.duration_us);
                const auto plan = hc::runtime::prepare_host_expansion(bounded, queues, host, "");
                const auto main_node = host.main.owned_node_ids.empty() ? host.main.owned_gap_slices.front().owner_node_id : host.main.owned_node_ids.front();
                row["rank"] = graph.node(main_node).gpu_id;
                row.update(expansion_observation(plan));
                row.update({
                    {         "status",          "observed" },
                    { "device_streams", plan.streams.size() },
                    {   "stream_waits",   plan.waits.size() }
                });
            }
            catch (const std::exception & error) {
                row.update({
                    { "status", "incomplete" },
                    { "reason", error.what() }
                });
            }
            rows.push_back(std::move(row));
        }
        publish({
            { "role", "independent_cost_audit" },
            { "rows",          std::move(rows) }
        });
        return 0;
    }
    return 2;
}

} // namespace

int main(int argc, char ** argv) {
    // manifest begin end output operation cpu-service [output operation ...]
    if (argc < 7 || argc % 2 == 0) return 2;
    std::vector<std::pair<std::string, std::string>> exports{
        { argv[4], argv[5] }
    };
    for (int index = 7; index < argc; index += 2) exports.emplace_back(argv[index], argv[index + 1]);
    for (const auto & [output, operation] : exports)
        if (std::ifstream(output).good()) throw std::runtime_error("Output exists; refusing to overwrite calibration");

    io::ManifestTraceInputOptions options;
    options.threads = options.file_threads = 2;
    options.window_start_us = std::stoull(argv[2]);
    options.window_end_us = std::stoull(argv[3]);
    auto inputs = io::load_trace_inputs_from_manifest(argv[1], options);
    auto graph = cli::build_input_graph(std::move(inputs), options.threads);
    if (std::string(argv[6]) != "-") {
        std::ifstream service(argv[6]);
        if (!service) throw std::runtime_error("Cannot open calibration CPU service file");
        graph.cpu_service_cost() = io::read_cpu_service_cost(service, argv[1]);
    }

    // Operation extraction may split CPU gaps. Each operation owns its copy;
    // the final one can consume the graph without another full allocation.
    for (size_t index = 0; index < exports.size(); ++index) {
        const auto & [output, operation] = exports[index];
        const auto status = export_operation(index + 1 == exports.size() ? std::move(graph) : graph,
                                             argv[1],
                                             *options.window_start_us,
                                             *options.window_end_us,
                                             output,
                                             operation,
                                             argv[6]);
        if (status != 0) return status;
    }
    return 0;
}
