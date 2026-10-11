// Offline extraction from an explicitly selected independent calibration only.
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/cost/hicache_submission.hpp"
#include "markov/trace_graph/io/cpu_service_input.hpp"
#include "markov/trace_graph/io/trace_manifest_input.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/model/prefetch_wait_execution.hpp"
#include "markov/trace_graph/modules/hicache/patch/layer_io.hpp"
#include "markov/trace_graph/modules/hicache/patch/layer_wait_insertion.hpp"
#include "markov/trace_graph/modules/hicache/runtime/control_calibration.hpp"
#include "markov/trace_graph/modules/hicache/runtime/load_execution.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include "markov/trace_graph/source_dag/build.hpp"
#include "markov/trace_graph/source_dag/observations.hpp"
#include "markov/trace_graph/source_dag/prefetch.hpp"
#include "markov/trace_graph/source_dag/submission_cost.hpp"
#include "markov/trace_graph/source_dag/tensor_cost.hpp"
#include "markov/trace_graph/structure/hicache/query.hpp"
#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <vector>
using namespace markov::trace_graph;
namespace hc = modules::hicache;

namespace {

// Local node ids are array offsets. CPU and device service share the same unit;
// residual CPU gaps and worker dispatch delays remain separate observations.
nlohmann::json expansion_observation(const hc::runtime::HiCacheHostExpansion & plan, const hc::patch::HiCacheSourceDagIndex & source,
                                     const simulation::detail::CpuTaskQueues & queues) {
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
        if (node.task_predecessor) item["task_predecessor"] = *node.task_predecessor;
        if (node.work.cpu_task_ready_delay_us) item["dispatch_us"] = *node.work.cpu_task_ready_delay_us;
        nodes.push_back(std::move(item));
    }

    // Expansion durations use integer simulation ticks. Calibration retains
    // the original fractional device measurement, not the rounded replay cost.
    // Event waits are measured separately with their synchronization proof.
    for (const auto & [original, local] : plan.source_nodes) {
        const auto & value = source.graph().node(original);
        if (value.is_cpu || value.kind != core::DagNodeKind::TraceEvent) continue;

        const auto & event = source.graph().event_for_node(original);
        if (event.name == "EVENT_WAIT" || value.duration != event.dur) continue;
        nodes[local]["service_us"] = static_cast<double>(event.dur) + static_cast<double>(event.dur_submicro_ns) / 1000.0;
    }

    auto edges = nlohmann::json::array();
    for (const auto & edge : plan.edges) edges.push_back({ edge.from, edge.to, static_cast<int>(edge.kind) });

    std::map<size_t, std::vector<size_t>> jobs;
    for (const auto & [original, local] : plan.source_nodes)
        if (plan.nodes[local].submission) jobs[*plan.nodes[local].submission].push_back(original);
    auto worker_costs = nlohmann::json::object();
    for (const auto & [submission, members] : jobs) {
        std::set<size_t> devices;
        for (const auto member : members)
            for (const auto edge_id : source.outgoing_edge_ids(member)) {
                const auto & edge = source.graph().edge(edge_id);
                if (edge.active && edge.kind == core::DagEdgeKind::Correlation && !source.graph().node(edge.dst).is_cpu) devices.insert(edge.dst);
            }
        if (const auto costs = source_dag::observe_worker_launch(source, queues, members, devices)) {
            worker_costs[std::to_string(submission)] = {
                {                 "worker_us",          costs->launch_us },
                {        "worker_residual_us", costs->launch_residual_us },
                {          "worker_return_us",          costs->return_us },
                { "worker_return_residual_us", costs->return_residual_us }
            };
        }
    }

    return {
        {        "nodes",        std::move(nodes) },
        {        "edges",        std::move(edges) },
        { "worker_costs", std::move(worker_costs) },
        {  "event_waits", plan.event_waits.size() }
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
        auto rows = source_dag::prefetch_query_observations(graph, rounds, hc::observe_prefetch_workers(source, rounds), begin_us, end_us);
        if (rows.empty()) throw std::runtime_error("Independent window has no complete query observations");
        publish({
            {                  "role","fixed_calibration"                                      },
            {             "operation",    "prefetch_query" },
            { "operator_observations",     std::move(rows) },
            {           "limitations",
             "All complete independent query groups; CPU correction only on measured spans. Scheduling and communication residuals retain source conditions; "
             "not pure CPU service."                      }
        });
        return 0;
    }
    if (operation == "write-host" || operation == "release-host") {
        if (!corrected) throw std::runtime_error("Shared write/release costs require paired CPU correction");
        const bool release_only = operation == "release-host";
        auto document = hc::runtime::export_write_calibration(graph, begin_us, end_us, release_only);
        // Write extraction splits boundaries. Its consumers need indices of
        // that graph, not the source snapshot taken before the split.
        const hc::patch::HiCacheSourceDagIndex calibrated(graph);
        const auto queues = simulation::detail::discover_cpu_task_queues(graph);
        for (const auto & observation : source_dag::confirmation_operator_observations(calibrated, queues, corrected))
            document["operator_observations"].push_back(observation);
        document.update({
            {        "role",                                                "fixed_calibration"                     },
            { "limitations",
             release_only
             ? "Observed ordinary-release program with paired CPU correction; residuals and device work retained. Nearest-size reuse is "
             "unvalidated extrapolation."
             : "Paired CPU correction; supported Ascend page-first submissions expose separate CPU, residual, worker, dispatch and event service. "
             "Payload service is excluded. Observed fragments remain resource evidence, not target structure rules." }
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
        auto operator_rows = nlohmann::json::array();
        std::set<size_t> check_rounds;
        if (corrected)
            for (const auto & [request, ranks] : grouped)
                for (const auto & [rank, calls] : ranks)
                    for (const auto & call : calls) {
                        if (!call.issue.empty()) continue;
                        if (call.state_round) check_rounds.insert(*call.state_round);
                        if (call.completion_round) check_rounds.insert(*call.completion_round);
                        const auto measured = source_dag::observe_prefetch_check_cpu(call, &source);
                        const auto * fact = source.fact_node(call.progress_fact);
                        const auto record = [&](std::string_view role, std::optional<uint64_t> duration_us) {
                            if (!duration_us) return;
                            const auto work = structure::hicache::prefetch_check_interval(role, rank, ranks.size());
                            operator_rows.push_back({
                                {       "position", "prefetch_check/" + std::to_string(fact->event_index) + "/" + std::string(role) },
                                {      "operation",                                                                  work.operation },
                                { "implementation",                                                             work.implementation },
                                {   "timing_scope",                                                               work.timing_scope },
                                {       "features",                                                                   work.features },
                                {    "duration_us",                                                                    *duration_us }
                            });
                        };

                        record("no_operation_return", measured.no_operation_return);
                        if (!call.local_return) record("entry_to_check", measured.entry_to_check);
                        record("check_to_max", measured.check_to_max);
                        record("max_to_false_return", measured.max_to_false_return);
                        record("max_to_stop", measured.max_to_stop);
                        record("best_effort_to_stop", measured.best_effort_to_stop);
                        record("stop_to_min", measured.stop_to_min);
                        record("min_to_visible", measured.min_to_visible);
                    }

        for (const auto id : check_rounds) {
            const auto & round = rounds.rounds.at(id);
            const auto measured = hc::observe_cpu_collective_timing(graph, round);
            if (!measured.issue.empty()) throw std::runtime_error("Independent prefetch communication: " + measured.issue);

            for (const auto & call : round.calls) {
                const auto & event = graph.runtime_observations().at(call.observation);
                if (event.arg("dtype") != "torch.int32") throw std::runtime_error("Prefetch check communication model requires int32 measurements");
                const bool completion = event.arg("role") == "prefetch_completion";
                const auto & timing = measured.calls.at(call.rank);
                const auto record = [&](std::string_view scope, uint64_t duration_us) {
                    const auto work = structure::hicache::prefetch_check_collective(completion, call.rank, round.members.size(), scope);
                    operator_rows.push_back({
                        {       "position", "runtime_collective/" + std::to_string(call.observation) + "/" + std::string(scope) },
                        {      "operation",                                                                      work.operation },
                        { "implementation",                                                                 work.implementation },
                        {   "timing_scope",                                                                   work.timing_scope },
                        {       "features",                                                                       work.features },
                        {    "duration_us",                                                                         duration_us }
                    });
                };

                record("before_submit_interval", timing.before_submit);
                record("submission_service", timing.submission);
                record("dispatch_interval", timing.dispatch);
                record("worker_remainder", timing.worker_remainder);
                record("after_join_interval", timing.after_join);
            }
        }

        size_t scheduler_intervals = 0;
        std::map<std::string, size_t> unavailable_intervals;
        for (const auto & [request, ranks] : grouped) {
            if (!corrected || policy == "best_effort" || ranks.empty()) continue;
            // Match ranks by the shared progress collective, not their local
            // call index. Measure every attributable retry rather than the
            // first request's first interval, which can retain startup work.
            for (const auto & previous : ranks.begin()->second) {
                if (!previous.issue.empty() || previous.local_return || previous.stop || !previous.state_round) continue;
                std::vector<hc::PrefetchControlObservation> first, next;
                for (const auto & [rank, calls] : ranks) {
                    const auto call = std::ranges::find(calls, previous.state_round, &hc::PrefetchControlObservation::state_round);
                    if (call == calls.end() || std::next(call) == calls.end()) break;
                    first.push_back(*call);
                    next.push_back(*std::next(call));
                }
                if (first.size() != ranks.size()) {
                    ++unavailable_intervals["missing_peer_progress"];
                    continue;
                }

                const auto observed = source_dag::observe_prefetch_scheduler_body(source, rounds, workers, first, next);
                if (!observed.issue.empty()) {
                    ++unavailable_intervals[observed.issue];
                    continue;
                }
                if (std::ranges::any_of(observed.steps, [&](const auto & step) {
                        return (step.action == hc::model::PrefetchSchedulerAction::WriteCompletion
                                || step.action == hc::model::PrefetchSchedulerAction::LoadCompletion)
                               && step.completion_observations.size() != ranks.size();
                    })) {
                    ++unavailable_intervals["missing_confirmation_boundary"];
                    continue;
                }

                const auto timing = source_dag::prefetch_scheduler_timing(observed, graph);
                ++scheduler_intervals;
                for (const auto & call : first) {
                    const auto rank = *call.rank;
                    const auto position = "scheduler/" + std::to_string(source.fact_node(call.progress_fact)->event_index);
                    const auto record = [&](const std::string & role, std::string_view scope, uint64_t duration_us, std::optional<int> root = std::nullopt) {
                        auto work = structure::hicache::prefetch_scheduler_operation(role, scope, rank, ranks.size());
                        if (root) work.features["root_rank"] = *root;
                        operator_rows.push_back({
                            {       "position", position + "/" + role + "/" + std::string(scope) },
                            {      "operation",                                   work.operation },
                            { "implementation",                              work.implementation },
                            {   "timing_scope",                                work.timing_scope },
                            {       "features",                                    work.features },
                            {    "duration_us",                                      duration_us }
                        });
                    };
                    const auto local = [&](const std::string & role, const hc::model::PrefetchSchedulerLocalTiming & measured) {
                        record(role, "cpu_service", measured.cpu_us);
                        record(role, "residual_interval", measured.gap_us);
                    };

                    for (const auto & step : observed.steps) {
                        const std::string name(hc::model::PREFETCH_SCHEDULER_NAMES.at(static_cast<size_t>(step.action)));
                        const auto & measured = timing.steps.at(step.action);
                        local(name + ".before", measured.before.at(rank));
                        local(name + ".after", measured.after.at(rank));
                        std::optional<int> root;
                        if (step.action == hc::model::PrefetchSchedulerAction::Receive) {
                            const auto & round = rounds.rounds.at(step.source_round);
                            const auto & event = graph.runtime_observations().at(round.calls.front().observation);
                            root = event.arg("group_src").empty() ? std::stoi(event.arg("src")) : round.members.at(std::stoul(event.arg("group_src")));
                        }
                        const auto & communication = measured.communication.calls.at(rank);
                        record(name + ".collective", "before_submit_interval", communication.before_submit, root);
                        record(name + ".collective", "submission_service", communication.submission, root);
                        record(name + ".collective", "dispatch_interval", communication.dispatch, root);
                        record(name + ".collective", "worker_remainder", communication.worker_remainder, root);
                        record(name + ".collective", "after_join_interval", communication.after_join, root);
                    }
                    local("tail", timing.tail.at(rank));
                }
            }
        }
        if (operator_rows.empty()) throw std::runtime_error("Independent capture has no complete prefetch measurements");
        publish({
            {                            "role",                                                                                                           "fixed_calibration" },
            {                       "operation",                                                                                                               "prefetch_wait" },
            {                   "source_policy",                                                                                                                        policy },
            {           "operator_observations",                                                                                                      std::move(operator_rows) },
            {             "scheduler_intervals",                                                                                                           scheduler_intervals },
            { "unavailable_scheduler_intervals",                                                                                                         unavailable_intervals },
            {                       "estimator", "All attributable progress calls and scheduler retry intervals; training balances captures. Target state determines retries." },
            {                     "limitations",    "CPU correction only where paired measurements exist; residual gaps, dispatch and worker timing retain source conditions." }
        });
        return 0;
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
                std::vector<size_t> worker_nodes;
                for (const auto & [original, local] : plan.source_nodes)
                    if (plan.nodes[local].submission) worker_nodes.push_back(original);
                const auto parts = source_dag::observe_worker_launch(source, queues, std::move(worker_nodes), { *call.device_wait });
                uint64_t before = 0, before_gap = 0, after = 0, after_gap = 0, worker = 0, dispatch = 0;
                size_t jobs = 0;
                for (size_t i = 0; i < plan.nodes.size(); ++i) {
                    const auto & node = plan.nodes[i];
                    if (!node.work.is_cpu) continue;
                    if (node.submission) {
                        worker += node.work.duration;
                        if (!node.task_predecessor) {
                            ++jobs;
                            if (!node.work.cpu_task_ready_delay_us) throw std::runtime_error("Layer wait worker has no observed dispatch interval");
                            dispatch += *node.work.cpu_task_ready_delay_us;
                        }
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
                    {            "status",                          "observed" },
                    {              "rank", graph.node(*call.submission).gpu_id },
                    {           "main_us",                              before },
                    {  "main_residual_us",                          before_gap },
                    {          "after_us",                               after },
                    { "after_residual_us",                           after_gap },
                    {         "worker_us",                              worker },
                    {       "dispatch_us",                            dispatch },
                    {         "device_us",                             nullptr }
                });
                if (const auto device = source_dag::observe_device_event_wait_service(source, *call.device_wait)) row["device_us"] = *device;
                else
                    row["device_service_issue"] = "No raw device event wait proven free of dependency waiting; normalized replay duration is not a measurement";
                if (parts) {
                    row["worker_launch_service_us"] = parts->launch_us;
                    row["worker_launch_residual_us"] = parts->launch_residual_us;
                    row["worker_return_service_us"] = parts->return_us;
                    row["worker_return_residual_us"] = parts->return_residual_us;
                }
                else { row["worker_split_issue"] = "No complete measurable single-launch task; aggregate is audit-only"; }
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
            { "rows", std::move(rows) },
            { "operator_observations", source_dag::layer_wait_operator_observations(source, queues, corrected) }
        });
        return 0;
    }
    if (operation == "audit-load-submission") {
        const auto ledger = hc::patch::build_hicache_io_operation_ledger(source);
        std::vector<core::TraceEvent> points;
        for (const auto & record : ledger.records)
            if (record.kind == hc::patch::HiCacheIoOperationKind::Load)
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
        auto operator_rows = nlohmann::json::array();
        for (const auto & record : ledger.records) {
            if (record.kind != hc::patch::HiCacheIoOperationKind::Load) continue;
            if (record.device_transfer_node_ids.empty()) {
                const auto host = hc::runtime::observe_host_template(bounded, record.pid, record.tid, record.source_start_us, record.source_end_us);
                const auto plan = hc::runtime::prepare_host_expansion(bounded, queues, host);
                if (!host.worker_nodes.empty() || !plan.streams.empty() || !plan.waits.empty() || !plan.event_waits.empty()) continue;
                uint64_t cpu_us = 0, residual_us = 0;
                for (const auto & node : plan.nodes) {
                    cpu_us += node.work.duration;
                    residual_us += node.work.cpu_gap_after;
                }
                for (const auto & [scope, duration] : std::vector<std::pair<std::string, uint64_t>>{
                         {                     corrected ? "cpu_service" : "profiled_cpu",      cpu_us },
                         { corrected ? "residual_interval" : "profiled_residual_interval", residual_us }
                }) {
                    operator_rows.push_back({
                        {       "position", "empty_load_queue:" + record.pid + ":" + record.tid + ":" + std::to_string(record.source_start_us) },
                        {      "operation",                                                                     "load.submission.empty_return" },
                        { "implementation",                                                                                      "empty_queue" },
                        {       "features",                                                                           nlohmann::json::object() },
                        {    "duration_us",                                                                                           duration },
                        {   "timing_scope",                                                                                              scope },
                    });
                }
                continue;
            }
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
                const auto costs = source_dag::observe_page_first_submission_cost(plan, bounded, queues, layers.size());
                if (!costs) throw std::runtime_error("Load submission cannot be partitioned into supported Ascend operations");
                const auto page_bytes = core::checked_add_u64(plan.payload.begin()->second, plan.payload.begin()->second, "Load page byte count overflow");
                const nlohmann::json features{
                    { "payload_bytes",              static_cast<double>(plan.payload_bytes) },
                    {    "page_bytes",                      static_cast<double>(page_bytes) },
                    {         "pages", static_cast<double>(plan.payload_bytes / page_bytes) },
                    {        "layers",                   static_cast<double>(layers.size()) }
                };
                using Cost = source_dag::PageFirstSubmissionMeasurement;
                for (const auto & [name, member] : {
                         std::pair{       "prepare_sync",            &Cost::prepare_sync },
                         {         "index_sync",              &Cost::index_sync },
                         {       "start_record",            &Cost::start_record },
                         {         "wait_event",              &Cost::wait_event },
                         {         "first_copy",              &Cost::first_copy },
                         {               "copy",                    &Cost::copy },
                         { "first_layer_record", &Cost::first_completion_record },
                         {       "layer_record",       &Cost::completion_record },
                         {               "tail",                    &Cost::tail }
                }) {
                    const auto & cost = *costs.*member;
                    for (const auto & [scope, duration] : std::vector<std::pair<std::string, std::optional<double>>>{
                             {                   "main_us",                   cost.main_us },
                             {          "main_residual_us",          cost.main_residual_us },
                             {                 "worker_us",                 cost.worker_us },
                             {        "worker_residual_us",        cost.worker_residual_us },
                             {          "worker_return_us",          cost.worker_return_us },
                             { "worker_return_residual_us", cost.worker_return_residual_us },
                             {               "dispatch_us",               cost.dispatch_us },
                             {                 "device_us",                 cost.device_us }
                    })
                        row["costs"][name][scope] = duration ? nlohmann::json(*duration) : nlohmann::json(nullptr);
                    if (std::string_view(name) == "layer_record" && layers.size() == 1) continue;
                    const auto emit = [&](std::string_view scope, std::optional<double> duration) {
                        if (!duration) return;
                        operator_rows.push_back({
                            {       "position", "load_submission:" + record.pid + ":" + record.tid + ":" + std::to_string(record.source_start_us) + ":" + name },
                            {      "operation",                                                                         "load.submission." + std::string(name) },
                            { "implementation",                                                                         "kernel_ascend.page_first_direct.load" },
                            {       "features",                                                                                                       features },
                            {    "duration_us",                                                                                                      *duration },
                            {   "timing_scope",                                                                                                          scope },
                        });
                    };
                    emit(corrected ? "cpu_service" : "profiled_cpu", cost.main_us);
                    emit(corrected ? "residual_interval" : "profiled_residual_interval", cost.main_residual_us);
                    if (std::string_view(name) == "start_record" || std::string_view(name) == "wait_event" || std::string_view(name) == "first_layer_record"
                        || std::string_view(name) == "layer_record") {
                        emit(corrected ? "worker_launch_service" : "profiled_worker_launch_service", cost.worker_us);
                        emit(corrected ? "worker_launch_residual_interval" : "profiled_worker_launch_residual_interval", cost.worker_residual_us);
                        emit(corrected ? "worker_return_service" : "profiled_worker_return_service", cost.worker_return_us);
                        emit(corrected ? "worker_return_residual_interval" : "profiled_worker_return_residual_interval", cost.worker_return_residual_us);
                        emit("dispatch_interval", cost.dispatch_us);
                        emit("device_service", cost.device_us);
                    }
                }
                row.update(expansion_observation(plan, bounded, queues));
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
            {                  "role", "independent_load_submission_audit" },
            {                  "rows",                     std::move(rows) },
            { "operator_observations",            std::move(operator_rows) }
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
        for (const auto & attempt : graph.runtime_observations()) {
            if (attempt.name != "runtime.hicache.load_allocation" && attempt.name != "runtime.hicache.load_admission"
                && attempt.name != "hicache.control.host_load_branch")
                continue;
            for (const auto at : { attempt.ts, attempt.ts + attempt.dur }) {
                core::TraceEvent point;
                point.name = "load allocator measurement boundary";
                point.pid = attempt.pid;
                point.tid = attempt.tid;
                point.ts = at;
                points.push_back(std::move(point));
            }
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
                {         "tid",                   fact.tid },
                {    "begin_us",          fact.timestamp_us },
                { "duration_us",           fact.duration_us }
            };
            try {
                const core::TraceEvent * allocation = nullptr;
                for (const auto & attempt : graph.runtime_observations()) {
                    if (attempt.name != "runtime.hicache.load_allocation" || attempt.pid != fact.pid || attempt.tid != fact.tid
                        || attempt.ts < fact.timestamp_us || attempt.ts + attempt.dur > fact.timestamp_us + fact.duration_us)
                        continue;
                    if (attempt.arg("status") != "returned" || attempt.arg("allocated") != "true") continue;
                    if (allocation) throw std::runtime_error("Index primitive measurements require one uniquely successful allocation");
                    allocation = &attempt;
                }
                if (!allocation || allocation->arg("status") != "returned" || allocation->arg("allocated") != "true"
                    || allocation->arg("allocator_need_sort") != "false" || allocation->arg_u64("requested_tokens") != fact.effective_token_count)
                    throw std::runtime_error("Index primitive measurements lack complete unsorted allocator work coordinates");
                for (const auto & admission : graph.runtime_observations()) {
                    if (admission.name != "runtime.hicache.load_admission" || admission.pid != fact.pid || admission.tid != fact.tid
                        || admission.ts > allocation->ts || admission.ts + admission.dur < allocation->ts + allocation->dur
                        || admission.arg("status") != "returned")
                        continue;
                    row["clone_tokens"] = nlohmann::json::parse(admission.arg("host_node_tokens"));
                }

                // Measure only the successful attempt and subsequent clones.
                // Earlier failed allocation/eviction work and host-index
                // preparation precede this boundary and have separate costs.
                const auto host = hc::runtime::observe_host_template(bounded, fact.pid, fact.tid, allocation->ts, fact.timestamp_us + fact.duration_us);
                const auto plan = hc::runtime::prepare_host_expansion(bounded, queues, host, "");
                const auto main_node = host.main.owned_node_ids.empty() ? host.main.owned_gap_slices.front().owner_node_id : host.main.owned_node_ids.front();
                row["rank"] = graph.node(main_node).gpu_id;
                row.update(expansion_observation(plan, bounded, queues));
                row.update({
                    {              "status",          "observed" },
                    { "allocation_begin_us",      allocation->ts },
                    {      "device_streams", plan.streams.size() },
                    {        "stream_waits",   plan.waits.size() }
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
        auto operator_observations = source_dag::load_admission_operator_observations(graph, corrected);
        for (const auto & observation : source_dag::prefix_concat_operator_observations(bounded, queues, corrected)) operator_observations.push_back(observation);

        publish({
            {                  "role",         "independent_cost_audit" },
            {                  "rows",                  std::move(rows) },
            { "operator_observations", std::move(operator_observations) }
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
    auto graph = source_dag::build(std::move(inputs), options.threads);
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
