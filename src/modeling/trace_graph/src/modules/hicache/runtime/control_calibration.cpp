#include "markov/trace_graph/modules/hicache/runtime/control_calibration.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/eviction_sequence.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_host.hpp"
#include "markov/trace_graph/modules/hicache/runtime/writes.hpp"
#include "markov/trace_graph/source_dag/submission_cost.hpp"
#include "source_dag/observation_writer.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>

namespace markov::trace_graph::modules::hicache::runtime {
using Json = nlohmann::json;

Json export_write_calibration(core::DagGraph & graph, uint64_t begin, uint64_t end, source_dag::ObservationWriter & observations, bool release_only,
                              std::string_view operation_prefix) {
    const adapters::sglang::HiCacheSourceIndex initial(graph);
    std::vector<core::TraceEvent> points;
    std::vector<std::pair<adapters::sglang::HiCacheFactObservation, HiCacheEvictionRegion>> regions;
    std::vector<std::tuple<std::string, std::string, uint64_t, uint64_t>> inactive_loads;
    const auto boundary = [&](const auto & fact, uint64_t first, uint64_t last) {
        for (const auto at : { first, last }) {
            core::TraceEvent point;
            point.name = "independent write boundary";
            point.pid = fact.pid;
            point.tid = fact.tid;
            point.ts = at;
            points.push_back(std::move(point));
        }
    };
    if (!release_only) {
        for (const auto & branch : graph.runtime_observations()) {
            if (branch.name != "hicache.control.host_load_branch" || branch.ts < begin || branch.ts + branch.dur > end) continue;

            const core::TraceEvent * condition = nullptr;
            for (const auto & event : graph.runtime_observations()) {
                if (event.name != "runtime.hicache.host_load_check" || event.pid != branch.pid || event.tid != branch.tid || event.ts < branch.ts
                    || event.ts + event.dur > branch.ts + branch.dur)
                    continue;
                if (condition) throw std::runtime_error("Independent load branch has multiple conditions");
                condition = &event;
            }
            if (!condition || condition->arg("status") != "returned" || condition->arg("needed") != "false") continue;

            const auto first = condition->ts + condition->dur;
            const auto last = branch.ts + branch.dur;
            if (first >= last) continue;

            inactive_loads.emplace_back(branch.pid, branch.tid, first, last);
            boundary(branch, first, last);
        }
    }
    for (const auto & fact : initial.fact_nodes()) {
        if (fact.phase != "end" || !fact.duration_us || fact.timestamp_us < begin || fact.timestamp_us + fact.duration_us > end) continue;
        if (!release_only && fact.fact_role == "commit_device_to_host_enqueue_observed")
            boundary(fact, fact.timestamp_us, fact.timestamp_us + fact.duration_us);
        if (fact.fact_role != "capacity_result_observed") continue;
        for (const auto & region : observe_eviction_regions(initial, fact)) {
            if (release_only && (region.kind != HiCacheEvictionRegion::Kind::ReleaseRegular || !region.released_tokens)) continue;
            boundary(fact, region.begin, region.end);
            if (region.kind != HiCacheEvictionRegion::Kind::WriteBackup) regions.emplace_back(fact, region);
        }
    }
    (void)bind_hicache_control_points(graph, points);
    const adapters::sglang::HiCacheSourceIndex source(graph);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    Json result{
        {              "rows", Json::array() },
        { "eviction_controls", Json::array() },
    };
    for (const auto & [pid, tid, first, last] : inactive_loads) {
        const auto host = observe_host_template(source, pid, tid, first, last);
        const auto plan = prepare_host_expansion(source, queues, host);
        if (!plan.streams.empty() || !plan.waits.empty() || !plan.event_waits.empty()
            || std::ranges::any_of(plan.nodes, [](const auto & node) { return !node.work.is_cpu || node.submission || node.queue_member; }))
            continue;

        double cpu_us = 0, residual_us = 0;
        for (const auto & node : plan.nodes) {
            cpu_us += node.work.duration;
            residual_us += node.work.cpu_gap_after;
        }
        const auto position = "load_noop:" + pid + ":" + tid + ":" + std::to_string(first);
        for (const auto & [scope, duration] : std::vector<std::pair<std::string, double>>{
                 {       "cpu_service",      cpu_us },
                 { "residual_interval", residual_us }
        }) {
            observations.push_back(
                {
                    {       "position",           position },
                    {      "operation", "load.noop_return" },
                    { "implementation",      "no_host_hit" },
                    {       "features",     Json::object() },
                    {    "duration_us",           duration },
                    {   "timing_scope",              scope },
            },
                operation_prefix);
        }
    }
    if (!release_only) {
        const auto ledger = patch::build_hicache_io_operation_ledger(source);
        model::HiCacheState state;
        HiCacheWrites writes(state);
        for (const auto & record : ledger.records) {
            if (record.kind != patch::HiCacheIoOperationKind::WriteDeviceToHost || record.source_start_us < begin || record.source_end_us > end) continue;
            writes.add_source(source, record);
            const auto host = observe_write_host_template(source, record);
            if (!host.write_back) throw std::runtime_error("Independent write has no observed policy");
            const auto plan = prepare_write_expansion(source, queues, record, host, writes.source_completion(record.timing_fact_node_id));
            const auto roles = write_resource_roles(graph, plan, writes.source_start_record(source, record.timing_fact_node_id));
            if (const auto costs = source_dag::observe_page_first_submission_cost(plan, source, queues, 1)) {
                using Cost = source_dag::PageFirstSubmissionMeasurement;
                // This extractor identifies the supported execution layout;
                // it does not provide topology to the predictor. Payload
                // duration is measured independently by the I/O provider.
                const auto half_page_bytes = plan.payload.begin()->second;
                const auto page_bytes = core::checked_add_u64(half_page_bytes, half_page_bytes, "Write page byte count overflow");
                const auto implementation = *host.write_back ? "kernel_ascend.page_first_direct.write_back" : "kernel_ascend.page_first_direct.write_through";
                for (const auto & [name, member, submitted] : std::vector<std::tuple<std::string, source_dag::HostOperationMeasurement Cost::*, bool>>{
                         {      "prepare_sync",            &Cost::prepare_sync, false },
                         {        "index_sync",              &Cost::index_sync, false },
                         {      "start_record",            &Cost::start_record,  true },
                         {        "wait_event",              &Cost::wait_event,  true },
                         {        "first_copy",              &Cost::first_copy, false },
                         {              "copy",                    &Cost::copy, false },
                         { "completion_record", &Cost::first_completion_record,  true },
                         {              "tail",                    &Cost::tail, false }
                }) {
                    const auto & primitive = *costs.*member;
                    const auto position = "write_submission:" + record.pid + ":" + record.tid + ":" + std::to_string(record.source_start_us) + ":" + name;
                    const structure::OperationWork work{
                        .operation = "write.submission." + name,
                        .implementation = implementation,
                        .features = { { "payload_bytes", static_cast<double>(plan.payload_bytes) },
                                     { "page_bytes", static_cast<double>(page_bytes) },
                                     { "pages", static_cast<double>(plan.payload_bytes / page_bytes) } },
                    };
                    observations.append(source_dag::host_operation_observations(work, position, primitive, true, submitted), operation_prefix);
                }
            }
            result["rows"].push_back({
                {     "status",                             "ready" },
                {       "rank", graph.node(roles.at("main")).gpu_id },
                { "write_back",                    *host.write_back },
                {   "begin_us",              record.source_start_us }
            });
        }
    }
    for (const auto & [fact, region] : regions) {
        const auto host = observe_host_template(source, fact.pid, fact.tid, region.begin, region.end);
        const auto plan = prepare_host_expansion(source, queues, host);
        if (release_only && !plan.event_waits.empty()) throw std::runtime_error("Ordinary release cannot wait for backup completion");
        const auto main = host.main.owned_node_ids.empty() ? host.main.owned_gap_slices.front().owner_node_id : host.main.owned_node_ids.front();
        if (plan.streams.size() > 1) throw std::runtime_error("Independent eviction has multiple compute resources");
        Json row{
            {          "status",                                "ready" },
            {            "rank",                graph.node(main).gpu_id },
            {            "kind",          static_cast<int>(region.kind) },
            {           "phase", static_cast<int>(region.control_phase) },
            { "released_tokens",                 region.released_tokens },
            {        "begin_us",                           region.begin }
        };
        if (region.kind == HiCacheEvictionRegion::Kind::Control) row["control_operation"] = eviction_control_name(region.control_phase);
        if (region.kind == HiCacheEvictionRegion::Kind::ReleaseRegular || region.kind == HiCacheEvictionRegion::Kind::ReleaseBackup) {
            const core::TraceEvent * allocation = nullptr;
            for (const auto & event : graph.runtime_observations()) {
                if (event.name != "runtime.hicache.allocator_free" || event.pid != fact.pid || event.tid != fact.tid || event.ts < region.begin
                    || event.ts + event.dur > region.end)
                    continue;
                if (allocation) throw std::runtime_error("Release has multiple allocator-free observations");
                allocation = &event;
            }
            // Old captures without tensor lengths are not evidence for the
            // concatenation work. Do not substitute their final free count.
            if (allocation && allocation->arg("immediate") == "true" && allocation->arg("status") == "returned"
                && !allocation->arg("release_buffer_pages").empty()) {
                const auto page_size = core::parse_u64(allocation->arg("page_size")).value();
                const auto tokens = core::parse_u64(allocation->arg("tokens")).value();
                const auto buffer = core::parse_u64(allocation->arg("release_buffer_pages")).value();
                const auto measured = source_dag::observe_paged_release_cost(plan, source, queues, page_size > 1);
                if (measured && page_size && tokens && tokens % page_size == 0) {
                    using Cost = source_dag::PagedReleaseMeasurement;
                    const auto implementation = std::string(page_size > 1 ? "ascend.paged_immediate." : "ascend.token_immediate.")
                                                + (region.kind == HiCacheEvictionRegion::Kind::ReleaseBackup ? "backup" : "regular");
                    for (const auto & [name, member, submitted] : std::vector<std::tuple<std::string, source_dag::HostOperationMeasurement Cost::*, bool>>{
                             {  "before_first_sync",  &Cost::before_first_sync, false },
                             {         "first_sync",         &Cost::first_sync, false },
                             { "before_second_sync", &Cost::before_second_sync, false },
                             {        "second_sync",        &Cost::second_sync, false },
                             {        "concatenate",        &Cost::concatenate,  true },
                             {               "tail",               &Cost::tail, false }
                    }) {
                        const auto & primitive = *measured.*member;
                        if (page_size == 1 && name != "concatenate" && name != "tail") continue;
                        const auto position = "release:" + fact.pid + ":" + fact.tid + ":" + std::to_string(region.begin) + ":" + name;
                        const structure::OperationWork work{
                            .operation = "release." + name,
                            .implementation = implementation,
                            .features = { { "released_pages", static_cast<double>(tokens / page_size) },
                                         { "page_size", static_cast<double>(page_size) },
                                         { "release_buffer_pages", static_cast<double>(buffer) },
                                         { "need_sort", allocation->arg("need_sort") == "true" ? 1.0 : 0.0 } },
                        };
                        observations.append(source_dag::host_operation_observations(work, position, primitive, true, submitted), operation_prefix);
                    }
                }
            }
        }
        // Only a same-thread CPU primitive can be aggregated this way. A
        // device/worker/wait fragment needs its own service decomposition.
        if (region.kind == HiCacheEvictionRegion::Kind::Control && plan.streams.empty() && plan.waits.empty() && plan.event_waits.empty()
            && std::ranges::all_of(plan.nodes, [&](const auto & node) {
                   return node.work.is_cpu && !node.submission && node.work.lane_key == graph.node_lane_key(main);
               })) {
            double cpu_us = 0, residual_us = 0;
            for (const auto & node : plan.nodes) {
                cpu_us += node.work.duration;
                residual_us += node.work.cpu_gap_after;
            }

            const auto position = "eviction:" + fact.pid + ":" + fact.tid + ":" + std::to_string(region.begin);
            for (const auto & [scope, duration] : std::vector<std::pair<std::string, double>>{
                     {       "cpu_service",      cpu_us },
                     { "residual_interval", residual_us }
            }) {
                observations.push_back(
                    {
                        {       "position",                                    position },
                        {      "operation",                          "eviction.control" },
                        { "implementation", eviction_control_name(region.control_phase) },
                        {       "features",                              Json::object() },
                        {    "duration_us",                                    duration },
                        {   "timing_scope",                                       scope },
                },
                    operation_prefix);
            }
        }
        if (region.preceding_kind)
            row["preceding_operation"] = {
                { "kind", static_cast<int>(*region.preceding_kind) }
            };
        result[release_only ? "rows" : "eviction_controls"].push_back(std::move(row));
    }
    if (release_only) {
        result["scope"] = "ordinary release resource evidence";
        result.erase("eviction_controls");
    }
    return result;
}


} // namespace markov::trace_graph::modules::hicache::runtime
