#include "markov/trace_graph/modules/hicache/runtime/write_calibration.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {
using Json = nlohmann::json;

Json export_write_calibration(core::DagGraph & graph, uint64_t begin, uint64_t end, bool release_only) {
    const patch::HiCacheSourceDagIndex initial(graph);
    std::vector<core::TraceEvent> points;
    std::vector<std::pair<patch::HiCacheSourceFactNode, HiCacheEvictionRegion>> regions;
    const auto boundary = [&](const auto & fact, uint64_t first, uint64_t last) {
        for (const auto at : {first, last}) {
            core::TraceEvent point;
            point.name = "independent write boundary";
            point.pid = fact.pid; point.tid = fact.tid; point.ts = at;
            points.push_back(std::move(point));
        }
    };
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
    const patch::HiCacheSourceDagIndex source(graph);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    Json result{
        {              "rows", Json::array() },
        { "eviction_controls", Json::array() }
    };
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
            Json submissions = Json::object();
            for (const auto & [role, node] : roles) {
                if (!role.starts_with("worker")) continue;
                std::set<std::string> names;
                for (const auto & work : plan.nodes) {
                    if (!work.queue_member || !work.submission || graph.node(*work.queue_member).lane_id != graph.node(node).lane_id) continue;
                    for (const auto & [original, local] : plan.source_nodes)
                        if (local == *work.submission) names.insert(graph.event_for_node(original).name);
                }
                if (names.empty()) throw std::runtime_error("Independent write worker has no submission identity");
                submissions[role] = names;
            }
            result["rows"].push_back({
                { "status", "ready" },
                { "rank", graph.node(roles.at("main")).gpu_id },
                { "write_back", *host.write_back },
                { "template", export_write_template(graph, plan, roles) },
                { "worker_submissions", submissions }
            });
        }
    }
    for (const auto & [fact, region] : regions) {
        const auto host = observe_host_template(source, fact.pid, fact.tid, region.begin, region.end);
        const auto plan = prepare_host_expansion(source, queues, host);
        if (release_only && !plan.event_waits.empty()) throw std::runtime_error("Ordinary release cannot wait for backup completion");
        const auto main = host.main.owned_node_ids.empty() ? host.main.owned_gap_slices.front().owner_node_id : host.main.owned_node_ids.front();
        if (plan.streams.size() > 1) throw std::runtime_error("Independent eviction has multiple compute resources");
        const auto roles =
            host_resource_roles(graph, plan, main, plan.streams.empty() ? std::nullopt : std::optional<size_t>{ plan.streams.front().source_node });
        Json row{
            { "status", "ready" },
            { "rank", graph.node(main).gpu_id },
            { "kind", static_cast<int>(region.kind) },
            { "phase", static_cast<int>(region.control_phase) },
            { "released_tokens", region.released_tokens },
            { "template", export_host_template(graph, plan, roles) }
        };
        if (region.preceding_kind)
            row["preceding_operation"] = {
                { "kind", static_cast<int>(*region.preceding_kind) }
            };
        result[release_only ? "rows" : "eviction_controls"].push_back(std::move(row));
    }
    if (result["rows"].empty())
        throw std::runtime_error(release_only ? "Independent capture has no complete ordinary release" : "Independent capture has no complete write operation");
    if (release_only) {
        result["scope"] = "ordinary release resource evidence";
        result.erase("eviction_controls");
    }
    return result;
}

void bind_calibration_workers(const Json & submissions, const core::DagGraph & base, const simulation::detail::CpuTaskQueues & queues,
                              std::map<std::string, size_t> & roles) {
    for (const auto & [role, names] : submissions.items()) {
        if (!role.starts_with("worker") || names.empty()) throw std::runtime_error("Calibration worker role lacks submission evidence");
        std::optional<size_t> worker;
        for (const auto & name : names) {
            const auto found = find_host_worker_resource(base, queues, roles.at("main"), name.get<std::string>());
            if (!found) throw std::runtime_error("Base lacks calibration worker submission: " + name.get<std::string>());
            if (worker && base.node(*worker).lane_id != base.node(*found).lane_id)
                throw std::runtime_error("Calibration worker submissions map to different base queues: " + role);
            worker = found;
        }
        if (const auto old = roles.find(role); old != roles.end() && base.node(old->second).lane_id != base.node(*worker).lane_id)
            throw std::runtime_error("Calibration worker evidence conflicts with existing resource: " + role);
        roles[role] = *worker;
    }
}

Json export_host_template(const core::DagGraph & graph, const HiCacheHostExpansion & plan,
                           const std::map<std::string, size_t> & roles) {
    std::map<std::string, std::string> lane_roles;
    for (const auto & [role, node] : roles) {
        const auto [at, inserted] = lane_roles.emplace(std::string(graph.node_lane_key(node)), role);
        if (!inserted && at->second != role) throw std::runtime_error("Portable write template needs unique resource roles");
    }
    const auto role_for = [&](size_t node) { return lane_roles.at(std::string(graph.node_lane_key(node))); };
    Json output{
        {       "nodes",    Json::array() },
        {       "edges",    Json::array() },
        {     "streams",    Json::array() },
        {       "waits",    Json::array() },
        { "event_waits",    Json::array() },
        { "host_return", plan.host_return }
    };
    for (const auto & node : plan.nodes) {
        const auto & work = node.work;
        if (!work.attrs.empty() || work.observed_point) throw std::runtime_error("Portable write work must not retain source coordinates or opaque attributes");
        Json row{{"name", work.name}, {"category", work.category}, {"is_cpu", work.is_cpu},
                 {"resource", lane_roles.at(work.lane_key)}, {"duration", work.duration},
                 {"cpu_gap_after", work.cpu_gap_after}, {"counts_toward_e2e", work.counts_toward_e2e}};
        if (node.submission) row["submission"] = *node.submission;
        if (node.queue_member) row["queue_resource"] = role_for(*node.queue_member);
        if (work.cpu_task_ready_delay_us) row["ready_delay_us"] = *work.cpu_task_ready_delay_us;
        output["nodes"].push_back(std::move(row));
    }
    for (const auto & edge : plan.edges) output["edges"].push_back({edge.from, edge.to, static_cast<int>(edge.kind)});
    for (const auto & stream : plan.streams) output["streams"].push_back({role_for(stream.source_node), stream.first, stream.last});
    for (const auto & wait : plan.waits) output["waits"].push_back({role_for(wait.source_node), wait.consumer});
    for (const auto & wait : plan.event_waits) output["event_waits"].push_back(wait.consumer);
    return output;
}

HiCacheHostExpansion import_host_template(const Json & input, const core::DagGraph & base, const simulation::detail::CpuTaskQueues & queues,
                                          const std::map<std::string, size_t> & roles) {
    HiCacheHostExpansion plan;
    std::optional<int> rank;
    const auto resource = [&](const Json & role) {
        const auto name = role.get<std::string>();
        const auto found = roles.find(name);
        if (found == roles.end()) throw std::runtime_error("Independent host template lacks a base resource: " + name);
        const auto & node = base.node(found->second);
        if (!node.active) throw std::runtime_error("Calibration role maps to an inactive resource");
        if (rank && *rank != node.gpu_id) throw std::runtime_error("Calibration host call crosses destination ranks");
        rank = node.gpu_id;
        return found->second;
    };

    for (const auto & row : input.at("nodes")) {
        HiCacheHostExpansion::Node node;
        auto & work = node.work;
        work.name = row.at("name");
        work.category = row.at("category");
        work.is_cpu = row.at("is_cpu");
        const auto id = resource(row.at("resource"));
        if (base.node(id).is_cpu != work.is_cpu) throw std::runtime_error("Calibration role maps to a wrong resource type");
        work.lane_key = std::string(base.node_lane_key(id));
        work.duration = row.at("duration");
        work.cpu_gap_after = row.at("cpu_gap_after");
        work.counts_toward_e2e = row.at("counts_toward_e2e");
        if (row.contains("submission")) node.submission = row.at("submission").get<size_t>();
        if (row.contains("queue_resource")) {
            const auto member = resource(row.at("queue_resource"));
            const auto task = queues.node_task.at(member);
            if (task == core::DagNode::kNoNode || base.node(member).lane_id != base.node(id).lane_id)
                throw std::runtime_error("Calibration worker requires a proven base queue on its mapped lane");
            node.queue_member = queues.tasks.at(task).first;
        }
        if (row.contains("ready_delay_us")) work.cpu_task_ready_delay_us = row.at("ready_delay_us").get<uint64_t>();
        plan.nodes.push_back(std::move(node));
    }

    for (const auto & edge : input.at("edges"))
        plan.edges.push_back({ edge.at(0).get<size_t>(), edge.at(1).get<size_t>(), static_cast<core::DagEdgeKind>(edge.at(2).get<int>()) });
    for (const auto & stream : input.at("streams")) plan.streams.push_back({ resource(stream.at(0)), stream.at(1).get<size_t>(), stream.at(2).get<size_t>() });
    for (const auto & wait : input.at("waits")) plan.waits.push_back({ resource(wait.at(0)), wait.at(1).get<size_t>() });
    // Target execution binds these completions; calibration event ids are not portable.
    for (const auto & wait : input.at("event_waits")) plan.event_waits.push_back({ core::DagNode::kNoNode, wait.get<size_t>() });

    plan.host_return = input.at("host_return");
    return plan;
}

Json export_write_template(const core::DagGraph & graph, const HiCacheWriteExpansion & plan, const std::map<std::string, size_t> & roles) {
    auto output = export_host_template(graph, plan, roles);
    output.update({
        {       "payload",       plan.payload },
        { "payload_bytes", plan.payload_bytes },
        {   "write_start",   plan.write_start },
        {    "completion",    plan.completion }
    });
    return output;
}

HiCacheWriteExpansion import_write_template(const Json & input, const core::DagGraph & base, const simulation::detail::CpuTaskQueues & queues,
                                            const std::map<std::string, size_t> & roles) {
    HiCacheWriteExpansion plan;
    static_cast<HiCacheHostExpansion &>(plan) = import_host_template(input, base, queues, roles);
    plan.payload = input.at("payload").get<decltype(plan.payload)>();
    plan.payload_bytes = input.at("payload_bytes");
    plan.write_start = input.at("write_start");
    plan.completion = input.at("completion");
    return plan;
}

} // namespace markov::trace_graph::modules::hicache::runtime
