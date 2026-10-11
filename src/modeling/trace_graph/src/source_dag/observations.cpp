#include "markov/trace_graph/source_dag/observations.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/fact.hpp"
#include "markov/trace_graph/modules/hicache/layer_waits.hpp"
#include "markov/trace_graph/modules/hicache/patch/source_dag_index.hpp"
#include "markov/trace_graph/modules/hicache/runtime/preparation.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include "markov/trace_graph/simulation/cpu_task_queues.hpp"
#include "markov/trace_graph/source_dag/confirmation.hpp"
#include "markov/trace_graph/source_dag/tensor_cost.hpp"
#include "markov/trace_graph/structure/ascend_event.hpp"
#include "markov/trace_graph/structure/hicache/confirmation.hpp"

#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace markov::trace_graph::source_dag {

std::optional<std::string> measurement_position(const core::TraceEvent & event) {
    if (!event.source_record_offset || event.source_channel == core::TraceSourceChannel::Synthetic) return std::nullopt;

    // File names and original record coordinates survive window filtering,
    // parallel parsing and input joins. Capture aliases must retain trace
    // file names; this does not deduplicate independently rewritten traces.
    // Include the measured interval: normalization may split a CPU envelope
    // into service pieces, which are not duplicate whole-record measurements.
    return std::to_string(static_cast<unsigned>(event.source_channel)) + ":" + *event.source_record_file + ":" + event.pid + ":" + event.tid + ":"
           + std::to_string(*event.source_record_offset) + ":" + std::to_string(event.ts) + ":" + std::to_string(event.ts_submicro_ns) + ":"
           + std::to_string(event.dur) + ":" + std::to_string(event.dur_submicro_ns);
}

nlohmann::json phase_operator_observations(const core::DagGraph & graph, const modules::hicache::HiCachePhaseObservationAudit & phases,
                                           bool cpu_service_applied, nlohmann::json * collective_diagnostics) {
    nlohmann::json rows = nlohmann::json::array();
    const modules::hicache::patch::HiCacheSourceDagIndex source_index(graph);
    // Device event instructions have one physical identity, independent of
    // whether HiCache or a collective submitted them. Export each raw event
    // once, retaining only waits whose producer was already complete.
    for (size_t node = 0; node < graph.node_count(); ++node) {
        const auto & entry = graph.node(node);
        if (!entry.active || entry.is_cpu || entry.kind != core::DagNodeKind::TraceEvent) continue;
        const auto & event = graph.event_for_node(node);
        if (event.name != "EVENT_RECORD" && event.name != "EVENT_WAIT") continue;
        const auto position = measurement_position(event);
        if (!position) continue;

        const bool wait = event.name == "EVENT_WAIT";
        const auto duration = wait ? observe_device_event_wait_service(source_index, node)
                                   : std::optional<double>{ static_cast<double>(event.dur) + static_cast<double>(event.dur_submicro_ns) / 1000.0 };
        if (!duration) continue;
        const auto work = structure::ascend_event_work("", wait ? structure::DeviceEventKind::Wait : structure::DeviceEventKind::Record);
        rows.push_back({
            {       "position",           *position },
            {      "operation",      work.operation },
            { "implementation", work.implementation },
            {       "features",       work.features },
            {    "duration_us",           *duration },
            {   "timing_scope",   work.timing_scope }
        });
    }

    // Raw phase membership is not per-operator work geometry. Only semantic
    // tensor/control exporters below may admit changed-work training samples.
    // Preparation owns its CPU spans and residual intervals, not phase
    // compute. Import only observations whose specialization and interval
    // ownership are known; a missing shape is not an inferred training point.
    const auto preparations = modules::hicache::runtime::observe_allocator_preparations(graph);
    std::map<std::pair<std::string, size_t>, std::string> preparation_positions;
    for (const auto & event : graph.hicache_fact_events()) {
        if (const auto position = measurement_position(event)) preparation_positions.emplace(std::pair{ event.pid, event.index }, *position);
    }
    for (const auto & call : preparations.calls) {
        if (!call.required || !call.specialization || call.path.empty() || !call.issues.empty() || !call.allocator_observation || call.intervals.empty())
            continue;
        const auto position = preparation_positions.find({ call.pid, call.source_fact_id });
        if (position == preparation_positions.end()) continue;

        // Compilation overlap is an observed outcome, not a deployment
        // feature that can be copied from a base run to target execution.
        const auto work = structure::hicache::allocator_preparation_operation({ "required", call.specialization, call.path, call.first_load });
        const auto & outer = graph.runtime_observations().at(*call.allocator_observation);
        nlohmann::json intervals = nlohmann::json::array();
        bool complete = true;
        for (const auto & [begin, end] : call.intervals) {
            if (!append_cpu_interval_observations(source_index, outer, work, begin, end, cpu_service_applied, intervals)) {
                complete = false;
                break;
            }
        }
        if (!complete) continue;

        // One invocation can contain compile and load intervals. Their union
        // is one preparation sample, not several labels for the full cost.
        // Keep CPU spans and residual intervals separate, as prediction does.
        std::map<std::string, double> durations;
        for (const auto & row : intervals) durations[row.at("timing_scope").get<std::string>()] += row.at("duration_us").get<double>();
        for (const auto & [scope, duration] : durations)
            rows.push_back({
                {       "position", "allocator:" + position->second },
                {      "operation",                  work.operation },
                { "implementation",             work.implementation },
                {       "features",                   work.features },
                {    "duration_us",                        duration },
                {   "timing_scope",                           scope },
            });
    }
    // A failed paged allocation without a release-queue merge returns before
    // tensor work. Sorted mode is admissible only when recorded queue counts
    // prove that the failed attempt did not execute Cat/Sort.
    // Thread CPU excludes descheduling, unlike the observed wall envelope.
    // Older captures without these scalar measurements remain unavailable.
    for (const auto & event : graph.runtime_observations()) {
        if (event.name != "runtime.hicache.load_allocation" || event.arg("status") != "returned" || event.arg("allocated") != "false"
            || !event.has_arg_key_hint("thread_cpu_ns"))
            continue;

        const bool sorted = event.arg("allocator_need_sort") == "true";
        if (sorted) {
            const auto free_pages = core::parse_u64(event.arg("free_pages_before"));
            const auto release_pages = core::parse_u64(event.arg("release_pages_before"));
            const auto page_size = event.arg_u64("page_size");
            if (!free_pages || !release_pages || !page_size || (event.arg_u64("requested_tokens") / page_size > *free_pages && *release_pages > 0)) continue;
        }
        else if (event.arg("allocator_need_sort") != "false") continue;

        const auto position = measurement_position(event);
        if (!position) continue;

        rows.push_back({
            {       "position","allocation_failure:" + *position                               },
            {      "operation",                                    "load.allocation_failure" },
            { "implementation",                   sorted ? "paged_sorted" : "paged_unsorted" },
            {       "features",
             {
             { "requested_tokens", static_cast<double>(event.arg_u64("requested_tokens")) },
             { "page_size_tokens", static_cast<double>(event.arg_u64("page_size")) },
             }                                                                              },
            {    "duration_us", static_cast<double>(event.arg_u64("thread_cpu_ns")) / 1000.0 },
            {   "timing_scope",                                         "thread_cpu_service" },
        });
    }
    for (const auto & row : confirmation_operator_observations(graph, cpu_service_applied)) rows.push_back(row);
    for (const auto & row : load_admission_operator_observations(graph, cpu_service_applied)) rows.push_back(row);
    for (const auto & row : prefix_concat_operator_observations(graph, cpu_service_applied)) rows.push_back(row);
    for (const auto & row : layer_wait_operator_observations(graph, cpu_service_applied)) rows.push_back(row);
    for (const auto & row : phase_tensor_operator_observations(graph, phases, cpu_service_applied)) rows.push_back(row);
    for (const auto & row : allocator_operator_observations(graph, cpu_service_applied)) rows.push_back(row);
    for (const auto & row : collective_operator_observations(graph, phases, cpu_service_applied, collective_diagnostics)) rows.push_back(row);
    return rows;
}

nlohmann::json layer_wait_operator_observations(const core::DagGraph & graph, bool cpu_service_applied) {
    nlohmann::json rows = nlohmann::json::array();
    const modules::hicache::patch::HiCacheSourceDagIndex source_index(graph);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    std::map<std::pair<std::string, uint64_t>, std::string> layer_positions;
    for (const auto & event : graph.runtime_observations()) {
        if (event.name != "runtime.hicache.layer_waits") continue;
        if (const auto position = measurement_position(event))
            layer_positions.emplace(std::pair{ event.pid, event.ts * 1'000 + event.ts_submicro_ns }, *position);
    }
    for (const auto & call : modules::hicache::observe_hicache_layer_waits(source_index).calls) {
        if (!call.issue.empty()) continue;
        const auto & event = graph.event_for_node(*call.before);
        const auto position = layer_positions.find({ event.pid, call.batch_start_ns });
        if (position == layer_positions.end()) continue;

        if (call.enabled) {
            const modules::hicache::runtime::HiCacheHostTemplate host{ .main = call.cpu, .worker_nodes = { *call.worker } };
            const auto plan = modules::hicache::runtime::prepare_host_expansion(source_index, queues, host);
            if (plan.streams.size() != 1 || plan.event_waits.size() != 1 || !plan.waits.empty()) continue;
            const auto submission = plan.source_nodes.at(*call.submission);
            double main_us = 0, main_residual_us = 0, tail_us = 0, tail_residual_us = 0, dispatch_us = 0;
            std::vector<size_t> worker_nodes;
            bool main_measurable = true, tail_measurable = true;
            for (const auto & [original, local] : plan.source_nodes) {
                const auto & node = plan.nodes[local];
                if (!node.work.is_cpu) continue;
                if (node.submission) {
                    worker_nodes.push_back(original);
                    continue;
                }

                const auto measured = observe_nonblocking_cpu_service(source_index, original);
                if (local <= submission) {
                    if (measured) main_us += *measured;
                    else main_measurable = false;
                }
                else {
                    if (measured) tail_us += *measured;
                    else tail_measurable = false;
                }
            }
            const auto worker = observe_worker_launch(source_index, queues, std::move(worker_nodes), { *call.device_wait });
            size_t jobs = 0;
            bool dispatch_observed = true;
            for (size_t i = 0; i < plan.nodes.size(); ++i) {
                const auto & node = plan.nodes[i];
                if (!node.work.is_cpu) continue;
                if (node.submission) {
                    if (!node.task_predecessor) {
                        ++jobs;
                        if (node.work.cpu_task_ready_delay_us) dispatch_us += *node.work.cpu_task_ready_delay_us;
                        else dispatch_observed = false;
                    }
                }
                else if (i <= submission) { main_residual_us += node.work.cpu_gap_after; }
                else { tail_residual_us += node.work.cpu_gap_after; }
            }
            if (jobs != 1 || !dispatch_observed) continue;
            const auto emit = [&](const std::string & operation, const std::string & scope, double duration) {
                rows.push_back({
                    {       "position",
                     "active_layer:" + position->second + ":" + std::to_string(call.layer) + ":" + std::to_string(call.position) + ":" + operation },
                    {      "operation",                                                                                                  operation },
                    { "implementation",                                                                                 "ascend.stream_wait_event" },
                    {       "features",                                                                                   nlohmann::json::object() },
                    {    "duration_us",                                                                                                   duration },
                    {   "timing_scope",                                                                                                      scope },
                });
            };
            if (main_measurable) {
                emit("layer_wait.submit", cpu_service_applied ? "cpu_service" : "profiled_cpu", main_us);
                emit("layer_wait.submit", cpu_service_applied ? "residual_interval" : "profiled_residual_interval", main_residual_us);
            }
            if (worker) {
                emit("layer_wait.submit", cpu_service_applied ? "worker_launch_service" : "profiled_worker_launch_service", worker->launch_us);
                emit("layer_wait.submit",
                     cpu_service_applied ? "worker_launch_residual_interval" : "profiled_worker_launch_residual_interval",
                     worker->launch_residual_us);
                emit("layer_wait.submit", cpu_service_applied ? "worker_return_service" : "profiled_worker_return_service", worker->return_us);
                emit("layer_wait.submit",
                     cpu_service_applied ? "worker_return_residual_interval" : "profiled_worker_return_residual_interval",
                     worker->return_residual_us);
            }
            emit("layer_wait.submit", cpu_service_applied ? "dispatch_interval" : "profiled_dispatch_interval", dispatch_us);
            if (tail_measurable) {
                emit("layer_wait.tail", cpu_service_applied ? "cpu_service" : "profiled_cpu", tail_us);
                emit("layer_wait.tail", cpu_service_applied ? "residual_interval" : "profiled_residual_interval", tail_residual_us);
            }
            continue;
        }

        // An inactive call contains no instrumented CPU leaf. Its Python
        // work remains in the residual interval, not a fabricated CPU sample.
        // Keep the simulator's microsecond coordinate convention, including
        // genuinely sub-tick observations; no minimum duration is imposed.
        const auto residual_us = graph.cpu_service_cost().duration({ event.pid, event.tid }, call.start_ns / 1'000, call.end_ns / 1'000);
        for (const auto & [scope, duration] : std::vector<std::pair<std::string, double>>{
                 {                     cpu_service_applied ? "cpu_service" : "profiled_cpu",                              0.0 },
                 { cpu_service_applied ? "residual_interval" : "profiled_residual_interval", static_cast<double>(residual_us) }
        }) {
            rows.push_back({
                {       "position", "inactive_layer:" + position->second + ":" + std::to_string(call.layer) + ":" + std::to_string(call.position) },
                {      "operation",                                                                                  "layer_wait.inactive_return" },
                { "implementation",                                                                                                    "inactive" },
                {       "features",                                                                                      nlohmann::json::object() },
                {    "duration_us",                                                                                                      duration },
                {   "timing_scope",                                                                                                         scope },
            });
        }
    }
    return rows;
}

namespace {

void append_blocking_write_observations(const modules::hicache::patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                        const core::TraceEvent & event, bool corrected, nlohmann::json & rows) {
    const auto before = core::parse_u64(event.arg("operations_before"));
    const auto after = core::parse_u64(event.arg("operations_after"));
    const auto batches_before = core::parse_u64(event.arg("batches_before"));
    const auto batches_after = core::parse_u64(event.arg("batches_after"));
    if (!before || !after || !batches_before || !batches_after || *after || *batches_after || *before != *batches_before) return;

    if (!*before) {
        const auto end = core::checked_add_u64(event.ts, event.dur, "Blocking write check end overflow");
        const auto program = structure::hicache::write_confirmation_program({}, true);
        (void)append_cpu_interval_observations(source, event, program.front().work, event.ts, end, corrected, rows);
        return;
    }

    const auto observation = static_cast<size_t>(&event - source.graph().runtime_observations().data());
    append_write_confirmation_observations(source, queues, observation, event.ts, *before, corrected, rows);
}

} // namespace

nlohmann::json confirmation_operator_observations(const core::DagGraph & graph, bool cpu_service_applied) {
    nlohmann::json rows = nlohmann::json::array();
    // Nonblocking confirmation costs are observed after the MIN returns;
    // empty blocking calls separately measure fixed entry/return work.
    // Queue deltas describe actual work; neither the old tail's local node
    // count nor its event-wait layout becomes the target's execution program.
    const modules::hicache::patch::HiCacheSourceDagIndex source_index(graph);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    for (const auto & event : graph.runtime_observations()) {
        if (event.name == "runtime.hicache.write_ack" && event.arg("status") == "returned") {
            const auto tokens = core::parse_u64(event.arg("backup_tokens"));
            const auto published = core::parse_u64(event.arg("published_nodes"));
            const auto storage = event.arg("storage_enabled");
            const auto unlock = event.arg("release_lock");
            if (!tokens || !published || (storage != "true" && storage != "false") || (unlock != "true" && unlock != "false")) continue;

            // This is the acknowledgement body, not synchronization or the
            // entire polling iteration. Capture-local ack_id is not a feature.
            const structure::OperationWork work{
                "hicache_io_control",
                "write.acknowledgement",
                "sglang.finish_write_through_ack",
                "cpu_service",
                { { "backup_tokens", static_cast<double>(*tokens) },
                  { "published_nodes", static_cast<double>(*published) },
                  { "storage_enabled", storage == "true" ? 1.0 : 0.0 },
                  { "release_lock", unlock == "true" ? 1.0 : 0.0 } }
            };
            (void)append_cpu_interval_observations(source_index,
                                                   event,
                                                   work,
                                                   event.ts,
                                                   core::checked_add_u64(event.ts, event.dur, "Write acknowledgement end overflow"),
                                                   cpu_service_applied,
                                                   rows);
            continue;
        }
        const bool write = event.name == "runtime.hicache.write_completion";
        if (write && event.arg("status") == "returned" && event.arg("blocking") == "true") {
            append_blocking_write_observations(source_index, queues, event, cpu_service_applied, rows);
            continue;
        }
        if ((!write && event.name != "runtime.hicache.load_completion") || event.arg("status") != "returned" || event.arg("blocking") != "false") continue;
        const auto position = measurement_position(event);
        if (!position) continue;

        const auto before = core::parse_u64(event.arg("operations_before"));
        const auto after = core::parse_u64(event.arg("operations_after"));
        const auto batches_before = core::parse_u64(event.arg("batches_before"));
        const auto batches_after = core::parse_u64(event.arg("batches_after"));
        if (!before || !after || !batches_before || !batches_after) continue;
        // The current controller model uses one operation per completion
        // batch. Other batching semantics need their own work description.
        if (*before < *after || *batches_before < *batches_after || *before - *after != *batches_before - *batches_after) continue;
        const auto role = write ? "write_completion_check" : "load_completion_check";
        const core::TraceEvent * reduction = nullptr;
        bool ambiguous = false;
        for (const auto & item : graph.runtime_observations()) {
            if (item.name != "runtime.cpu_collective" || item.arg("role") != role || item.pid != event.pid || item.tid != event.tid || item.ts < event.ts
                || item.ts + item.dur > event.ts + event.dur)
                continue;
            if (reduction) ambiguous = true;
            reduction = &item;
        }
        if (!reduction || ambiguous || reduction->arg("status") != "returned") continue;
        const auto first_us = reduction->ts + reduction->dur;
        const auto end_us = event.ts + event.dur;
        if (first_us >= end_us) continue;

        if (write && *before > *after) {
            const auto observation = static_cast<size_t>(&event - graph.runtime_observations().data());
            append_write_confirmation_observations(source_index, queues, observation, first_us, *before - *after, cpu_service_applied, rows);
            continue;
        }

        const structure::OperationWork work{ "hicache_io_control",
                                             write ? "write.confirmation_tail" : "load.confirmation_tail",
                                             "nonblocking",
                                             "cpu_service",
                                             { { "confirmed_operations", static_cast<double>(*before - *after) } } };
        const auto first_row = rows.size();
        (void)append_cpu_interval_observations(source_index, event, work, first_us, end_us, cpu_service_applied, rows);
        // Keep the original measurement identity: stricter service extraction
        // must not make reimports look like additional independent samples.
        for (size_t index = first_row; index < rows.size(); ++index) rows[index]["position"] = "confirmation_tail:" + *position;
    }
    return rows;
}

nlohmann::json load_admission_operator_observations(const core::DagGraph & input, bool cpu_service_applied) {
    nlohmann::json rows = nlohmann::json::array();
    std::vector<core::TraceEvent> points;
    const auto append_boundaries = [&](const core::TraceEvent & event) {
        for (const auto at : { event.ts, core::checked_add_u64(event.ts, event.dur, "Load measurement end overflow") }) {
            core::TraceEvent point;
            point.name = "independent load cost boundary";
            point.pid = event.pid;
            point.tid = event.tid;
            point.ts = at;
            points.push_back(std::move(point));
        }
    };
    for (const auto & event : input.runtime_observations())
        if (event.name == "runtime.hicache.load_allocation" || event.name == "runtime.hicache.load_admission"
            || event.name == "hicache.control.host_load_branch")
            append_boundaries(event);
    for (const auto & event : input.hicache_fact_events())
        if (event.arg("phase") == "end" && modules::hicache::parse_hicache_fact_metadata(event).role == "loadback_decision_observed") append_boundaries(event);
    if (points.empty()) return rows;

    // Known parent self-time can straddle these measurement coordinates.
    // Split it on a private graph, using the same ownership rules as the
    // independent exporter. Never change the source graph or relax overlaps.
    auto graph = input;
    (void)modules::hicache::bind_hicache_control_points(graph, points);
    const modules::hicache::patch::HiCacheSourceDagIndex source(graph);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    for (const auto & event : graph.runtime_observations()) {
        if (event.name != "runtime.hicache.load_admission" || event.arg("status") != "returned") continue;
        const auto position = measurement_position(event);
        if (!position) continue;
        const auto requested_tokens = event.arg_u64("requested_tokens");
        const auto page_size = event.arg_u64("page_size");
        if (!requested_tokens || !page_size || requested_tokens % page_size) continue;
        const auto node_tokens = nlohmann::json::parse(event.arg("host_node_tokens"));

        const core::TraceEvent * first = nullptr;
        const core::TraceEvent * last = nullptr;
        for (const auto & attempt : graph.runtime_observations()) {
            if (attempt.name != "runtime.hicache.load_allocation" || attempt.pid != event.pid || attempt.tid != event.tid || attempt.ts < event.ts
                || attempt.ts + attempt.dur > event.ts + event.dur)
                continue;
            if (!first || attempt.ts < first->ts) first = &attempt;
            if (!last || attempt.ts > last->ts) last = &attempt;
        }
        // Only the final failed attempt owns failure cleanup. Eviction and
        // earlier retries precede this boundary and must not enter its cost.
        const auto returned_tokens = core::parse_u64(event.arg("returned_tokens"));
        const auto threshold_tokens = core::parse_u64(event.arg("load_back_threshold_tokens"));
        if (!first && returned_tokens && *returned_tokens == 0 && threshold_tokens && requested_tokens < *threshold_tokens
            && event.has_arg_key_hint("mem_quota_tokens") && event.arg("mem_quota_tokens").empty()) {
            const auto ownership = source.timing_interval_ownership(event.pid, event.tid, event.ts, event.dur);
            if (ownership.status == "ready" && !ownership.has_node_overlap) {
                double cpu_us = 0, residual_us = 0;
                for (const auto node : ownership.owned_node_ids) cpu_us += graph.cpu_service_node_duration(node);
                for (const auto & gap : ownership.owned_gap_slices)
                    residual_us += graph.cpu_service_cost().duration({ event.pid, event.tid }, gap.owned_start_us, gap.owned_end_us);

                for (const auto & [scope, duration] : std::vector<std::pair<std::string, double>>{
                         {                     cpu_service_applied ? "cpu_service" : "profiled_cpu",      cpu_us },
                         { cpu_service_applied ? "residual_interval" : "profiled_residual_interval", residual_us }
                }) {
                    rows.push_back({
                        {       "position","load_threshold_skip:" + *position                                           },
                        {      "operation",                           "load.admission.skip" },
                        { "implementation",                         "below_token_threshold" },
                        {       "features",
                         { { "requested_tokens", static_cast<double>(requested_tokens) },
                         { "page_size_tokens", static_cast<double>(page_size) },
                         { "pages", static_cast<double>(requested_tokens / page_size) },
                         { "promoted_nodes", static_cast<double>(node_tokens.size()) } }   },
                        {    "duration_us",                                        duration },
                        {   "timing_scope",                                           scope },
                    });
                }
            }
        }
        if (last && returned_tokens && *returned_tokens == 0 && last->arg("status") == "returned" && last->arg("allocated") == "false"
            && last->arg_u64("requested_tokens") == requested_tokens) {
            const auto begin_us = last->ts + last->dur;
            const auto end_us = event.ts + event.dur;
            const auto tail = source.timing_interval_ownership(event.pid, event.tid, begin_us, end_us - begin_us);
            if (end_us > begin_us && tail.status == "ready" && !tail.has_node_overlap) {
                const auto host = modules::hicache::runtime::observe_host_template(source, event.pid, event.tid, begin_us, end_us);
                const auto plan = modules::hicache::runtime::prepare_host_expansion(source, queues, host);
                bool cpu_only = plan.streams.empty() && plan.waits.empty() && plan.event_waits.empty();
                for (const auto & node : plan.nodes) cpu_only = cpu_only && node.work.is_cpu && !node.queue_member;

                if (cpu_only) {
                    double cpu_us = 0, residual_us = 0;
                    for (const auto node : tail.owned_node_ids) cpu_us += graph.cpu_service_node_duration(node);
                    for (const auto & gap : tail.owned_gap_slices)
                        residual_us += graph.cpu_service_cost().duration({ event.pid, event.tid }, gap.owned_start_us, gap.owned_end_us);
                    for (const auto & [scope, duration] : std::vector<std::pair<std::string, double>>{
                             {                     cpu_service_applied ? "cpu_service" : "profiled_cpu",      cpu_us },
                             { cpu_service_applied ? "residual_interval" : "profiled_residual_interval", residual_us }
                    }) {
                        rows.push_back({
                            {       "position","load_failure_tail:" + *position                                               },
                            {      "operation",                        "load.index.failure_tail" },
                            { "implementation",                          "ascend.paged_unsorted" },
                            {       "features",
                             { { "requested_tokens", static_cast<double>(requested_tokens) },
                             { "page_size_tokens", static_cast<double>(page_size) },
                             { "pages", static_cast<double>(requested_tokens / page_size) } }   },
                            {    "duration_us",                                         duration },
                            {   "timing_scope",                                            scope },
                        });
                    }
                }
            }
        }

        // Threshold/quota returns without an allocator call are a distinct
        // execution branch, not observations of allocation preparation.
        if (!first || first->ts <= event.ts) continue;
        const auto ownership = source.timing_interval_ownership(event.pid, event.tid, event.ts, first->ts - event.ts);
        if (ownership.status != "ready" || ownership.has_node_overlap) continue;
        double cpu_us = 0, residual_us = 0;
        for (const auto node : ownership.owned_node_ids) cpu_us += graph.cpu_service_node_duration(node);
        for (const auto & gap : ownership.owned_gap_slices)
            residual_us += graph.cpu_service_cost().duration({ event.pid, event.tid }, gap.owned_start_us, gap.owned_end_us);

        for (const auto & [scope, duration] : std::vector<std::pair<std::string, double>>{
                 {                     cpu_service_applied ? "cpu_service" : "profiled_cpu",      cpu_us },
                 { cpu_service_applied ? "residual_interval" : "profiled_residual_interval", residual_us }
        }) {
            rows.push_back({
                {       "position","load_prepare:" + *position                                   },
                {      "operation",                        "load.admission.prepare" },
                { "implementation",                              "host_indices_cpu" },
                {       "features",
                 { { "requested_tokens", static_cast<double>(requested_tokens) },
                 { "page_size_tokens", static_cast<double>(page_size) },
                 { "pages", static_cast<double>(requested_tokens / page_size) },
                 { "promoted_nodes", static_cast<double>(node_tokens.size()) } }   },
                {    "duration_us",                                        duration },
                {   "timing_scope",                                           scope },
            });
        }
    }
    return rows;
}

nlohmann::json prefix_concat_operator_observations(const core::DagGraph & graph, bool cpu_service_applied) {
    nlohmann::json rows = nlohmann::json::array();
    const modules::hicache::patch::HiCacheSourceDagIndex source(graph);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    for (const auto & admission : graph.runtime_observations()) {
        if (admission.name != "runtime.hicache.load_admission" || admission.arg("status") != "returned") continue;
        const auto position = measurement_position(admission);
        const auto prefix_tokens = core::parse_u64(admission.arg("existing_prefix_tokens"));
        const auto new_tokens = core::parse_u64(admission.arg("returned_tokens"));
        if (!position || !prefix_tokens || !new_tokens) continue;

        const core::TraceEvent * branch = nullptr;
        bool ambiguous = false;
        for (const auto & event : graph.runtime_observations()) {
            if (event.name != "hicache.control.host_load_branch" || event.pid != admission.pid || event.tid != admission.tid || event.ts > admission.ts
                || event.ts + event.dur < admission.ts + admission.dur)
                continue;
            if (branch) ambiguous = true;
            branch = &event;
        }
        if (!branch || ambiguous) continue;
        const auto first_us = admission.ts + admission.dur;
        const auto end_us = branch->ts + branch->dur;
        if (end_us <= first_us) continue;
        const auto ownership = source.timing_interval_ownership(admission.pid, admission.tid, first_us, end_us - first_us);
        if (ownership.status != "ready" || ownership.has_node_overlap) continue;
        const auto host = modules::hicache::runtime::observe_host_template(source, admission.pid, admission.tid, first_us, end_us);
        const auto plan = modules::hicache::runtime::prepare_host_expansion(source, queues, host);
        if (!plan.waits.empty() || !plan.event_waits.empty()) continue;

        const auto output_tokens = core::checked_add_u64(*prefix_tokens, *new_tokens, "Observed prefix token count overflow");
        const nlohmann::json features{
            {   "prefix_tokens",                           static_cast<double>(*prefix_tokens) },
            {      "new_tokens",                              static_cast<double>(*new_tokens) },
            {   "output_tokens",                            static_cast<double>(output_tokens) },
            { "nonempty_inputs", static_cast<double>((*prefix_tokens > 0) + (*new_tokens > 0)) }
        };
        const auto emit = [&](const std::string & operation, const std::string & scope, double duration) {
            rows.push_back({
                {       "position", "prefix_concat:" + *position + ":" + operation },
                {      "operation",                                      operation },
                { "implementation",                             "ascend.aclnn_cat" },
                {       "features",                                       features },
                {    "duration_us",                                       duration },
                {   "timing_scope",                                          scope },
            });
        };
        std::vector<size_t> workers, devices;
        for (size_t i = 0; i < plan.nodes.size(); ++i) {
            if (plan.nodes[i].queue_member) workers.push_back(i);
            if (!plan.nodes[i].work.is_cpu) devices.push_back(i);
        }
        if (!output_tokens) {
            if (!workers.empty() || !devices.empty() || !plan.streams.empty()) continue;
            double cpu_us = 0, residual_us = 0;
            bool measurable = true;
            for (const auto & [original, local] : plan.source_nodes) {
                const auto measured = observe_nonblocking_cpu_service(source, original);
                if (!measured) {
                    measurable = false;
                    break;
                }
                cpu_us += *measured;
            }
            if (!measurable) continue;
            for (const auto & node : plan.nodes) residual_us += node.work.cpu_gap_after;
            emit("load.admission.prefix_concat", cpu_service_applied ? "cpu_service" : "profiled_cpu", cpu_us);
            emit("load.admission.prefix_concat", cpu_service_applied ? "residual_interval" : "profiled_residual_interval", residual_us);
            continue;
        }
        if (workers.empty() || devices.size() != 1 || plan.streams.size() != 1) continue;
        std::vector<size_t> task_heads;
        for (const auto node : workers)
            if (!plan.nodes[node].task_predecessor) task_heads.push_back(node);
        if (task_heads.size() != 1) continue;
        const auto & worker = plan.nodes[task_heads.front()];
        if (!worker.submission || !worker.work.cpu_task_ready_delay_us || !plan.nodes[*worker.submission].work.name.ends_with("Enqueue@aclnnCat")) continue;

        std::vector<size_t> original_workers;
        std::set<size_t> original_devices;
        double cpu_us = 0, residual_us = 0, tail_cpu_us = 0, tail_residual_us = 0;
        bool main_measurable = true, tail_measurable = true;
        for (const auto & [original, local] : plan.source_nodes) {
            const auto & node = plan.nodes[local];
            if (!node.work.is_cpu) {
                original_devices.insert(original);
                continue;
            }
            if (node.queue_member) {
                original_workers.push_back(original);
                continue;
            }

            const auto measured = observe_nonblocking_cpu_service(source, original);
            if (local <= *worker.submission) {
                if (measured) cpu_us += *measured;
                else main_measurable = false;
            }
            else {
                if (measured) tail_cpu_us += *measured;
                else tail_measurable = false;
            }
        }
        for (size_t i = 0; i < plan.nodes.size(); ++i) {
            const auto & node = plan.nodes[i];
            if (!node.work.is_cpu || node.queue_member) continue;
            if (i <= *worker.submission) residual_us += node.work.cpu_gap_after;
            else tail_residual_us += node.work.cpu_gap_after;
        }
        if (main_measurable) {
            emit("load.admission.prefix_concat", cpu_service_applied ? "cpu_service" : "profiled_cpu", cpu_us);
            emit("load.admission.prefix_concat", cpu_service_applied ? "residual_interval" : "profiled_residual_interval", residual_us);
        }
        if (const auto measured = observe_worker_launch(source, queues, std::move(original_workers), original_devices)) {
            emit("load.admission.prefix_concat", cpu_service_applied ? "worker_launch_service" : "profiled_worker_launch_service", measured->launch_us);
            emit("load.admission.prefix_concat",
                 cpu_service_applied ? "worker_launch_residual_interval" : "profiled_worker_launch_residual_interval",
                 measured->launch_residual_us);
            emit("load.admission.prefix_concat", cpu_service_applied ? "worker_return_service" : "profiled_worker_return_service", measured->return_us);
            emit("load.admission.prefix_concat",
                 cpu_service_applied ? "worker_return_residual_interval" : "profiled_worker_return_residual_interval",
                 measured->return_residual_us);
        }
        emit("load.admission.prefix_concat",
             cpu_service_applied ? "dispatch_interval" : "profiled_dispatch_interval",
             worker.work.cpu_task_ready_delay_us.value());
        if (original_devices.size() == 1) {
            const auto device = *original_devices.begin();
            const auto & event = graph.event_for_node(device);
            if (graph.node(device).kind == core::DagNodeKind::TraceEvent && graph.node(device).duration == event.dur)
                emit("load.admission.prefix_concat", "device_service", static_cast<double>(event.dur) + static_cast<double>(event.dur_submicro_ns) / 1000.0);
        }
        if (tail_measurable) {
            emit("load.admission.prefix_tail", cpu_service_applied ? "cpu_service" : "profiled_cpu", tail_cpu_us);
            emit("load.admission.prefix_tail", cpu_service_applied ? "residual_interval" : "profiled_residual_interval", tail_residual_us);
        }
    }
    return rows;
}

} // namespace markov::trace_graph::source_dag
