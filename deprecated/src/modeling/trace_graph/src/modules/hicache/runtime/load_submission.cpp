#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/loads.hpp"

#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

void HiCacheLoads::prepare_generated_submissions(core::DagGraph & graph) {
    const patch::HiCacheSourceDagIndex initial(graph);
    std::vector<std::pair<patch::HiCacheSourceFactNode, HiCacheEvictionRegion>> releases;
    std::vector<core::TraceEvent> points;
    for (const auto & fact : initial.fact_nodes()) {
        if (fact.fact_role != "capacity_result_observed" || fact.phase != "end") continue;
        for (const auto & region : observe_eviction_regions(initial, fact)) {
            if (region.kind != HiCacheEvictionRegion::Kind::ReleaseRegular && region.kind != HiCacheEvictionRegion::Kind::ReleaseBackup) continue;
            releases.emplace_back(fact, region);
            for (const auto at : { region.begin, region.end }) {
                core::TraceEvent point;
                point.name = "load compute role boundary";
                point.pid = fact.pid;
                point.tid = fact.tid;
                point.ts = at;
                points.push_back(std::move(point));
            }
        }
    }
    (void)bind_hicache_control_points(graph, points);
    const patch::HiCacheSourceDagIndex source(graph);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    auto compute_roles = compute_resources_;
    const auto witness = [&](const std::string & pid, const std::string & tid, size_t node, std::string_view origin) {
        const auto [at, inserted] = compute_roles.emplace(std::pair{ pid, tid }, node);
        if (!inserted && graph.node(at->second).lane_id != graph.node(node).lane_id)
            throw std::runtime_error("Scheduler load submission has conflicting compute roles: " + pid + "/" + tid + " origin=" + std::string(origin)
                                     + " existing=" + std::string(graph.node_lane_key(at->second)) + ":" + graph.event_for_node(at->second).name
                                     + " observed=" + std::string(graph.node_lane_key(node)) + ":" + graph.event_for_node(node).name);
    };
    model::HiCacheState resource_state;
    HiCacheWrites writes(resource_state);
    for (const auto & record : patch::build_hicache_io_operation_ledger(source).records) {
        if (record.kind != patch::HiCacheIoOperationKind::WriteDeviceToHost) continue;
        writes.add_source(source, record);
        witness(record.pid, record.tid, writes.source_start_record(source, record.timing_fact_node_id), "write_start");
    }
    for (const auto & [fact, region] : releases) {
        const auto host = observe_host_template(source, fact.pid, fact.tid, region.begin, region.end);
        const auto plan = prepare_host_expansion(source, queues, host);
        if (plan.streams.size() == 1) witness(fact.pid, fact.tid, plan.streams.front().source_node, "release");
    }
    for (const auto & load : loads_) {
        if (load.layers.empty()) continue;
        const auto & record = load.record;
        const auto host = observe_host_template(source, record.pid, record.tid, record.source_start_us, record.source_end_us);
        const auto plan = prepare_write_expansion(source, queues, record, host, load.layers.back().source_record);
        // A nonempty source load also proves its compute role through the
        // index readback barrier. Only resource identity is reused here.
        if (plan.waits.size() == 1) witness(record.pid, record.tid, plan.waits.front().source_node, "load_index_barrier");
    }
    std::map<std::string, std::set<uint64_t>> layers;
    for (const auto & call : observe_hicache_layer_waits(source).calls)
        if (call.before) layers[graph.event_for_node(*call.before).pid].insert(call.layer);
    for (const auto & load : loads_) {
        const auto key = std::pair{ load.fact.pid, load.fact.tid };
        if (generated_submissions_.contains(key) || !compute_roles.contains(key)) continue;
        const auto main = source.cpu_boundary_at_or_before(key.first, key.second, load.record.source_start_us);
        if (!main) continue;
        const auto resources = observe_load_index_resources(source, queues, *main, compute_roles.at(key));
        if (!resources) continue;
        const auto & model_layers = layers[key.first];
        if (model_layers.empty() || *model_layers.begin() != 0 || *model_layers.rbegin() + 1 != model_layers.size())
            throw std::runtime_error("Generated load requires the base model's complete layer identities");
        const auto rank = graph.node(*main).gpu_id;
        // An identity for a target-only resource, not a zero-cost executable op.
        const auto stream = graph.add_device_resource("hicache.generated_load:" + key.first + ":" + key.second, rank);
        generated_submissions_.emplace(key, GeneratedSubmission{ *main, resources->worker, resources->compute, stream, model_layers.size() });
    }
}

} // namespace markov::trace_graph::modules::hicache::runtime
