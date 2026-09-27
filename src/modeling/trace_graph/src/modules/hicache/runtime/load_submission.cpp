#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/control_calibration.hpp"
#include "markov/trace_graph/modules/hicache/runtime/loads.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

void HiCacheLoads::prepare_generated_submissions(core::DagGraph & graph) {
    if (submission_calibration_.empty()) return;
    const auto document = read_control_calibration(submission_calibration_, "load_submission");
    if (document.at("backend") != "kernel_ascend" || document.at("layout") != "page_first_direct")
        throw std::runtime_error("Generated load requires independent Ascend page-first submission costs");
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
    std::map<std::pair<std::string, std::string>, size_t> compute_roles;
    const auto witness = [&](const std::string & pid, const std::string & tid, size_t node) {
        const auto [at, inserted] = compute_roles.emplace(std::pair{ pid, tid }, node);
        if (!inserted && graph.node(at->second).lane_id != graph.node(node).lane_id)
            throw std::runtime_error("Scheduler load submission has conflicting compute roles");
    };
    model::HiCacheState resource_state;
    HiCacheWrites writes(resource_state);
    for (const auto & record : patch::build_hicache_io_operation_ledger(source).records) {
        if (record.kind != patch::HiCacheIoOperationKind::WriteDeviceToHost) continue;
        writes.add_source(source, record);
        witness(record.pid, record.tid, writes.source_start_record(source, record.timing_fact_node_id));
    }
    for (const auto & [fact, region] : releases) {
        const auto host = observe_host_template(source, fact.pid, fact.tid, region.begin, region.end);
        const auto plan = prepare_host_expansion(source, queues, host);
        if (plan.streams.size() == 1) witness(fact.pid, fact.tid, plan.streams.front().source_node);
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
        const nlohmann::json * row = nullptr;
        for (const auto & sample : document.at("ranks"))
            if (sample.at("rank").get<int>() == rank) {
                if (row) throw std::runtime_error("Duplicate load submission calibration rank");
                row = &sample;
            }
        if (!row) continue;
        const auto & costs = row->at("costs");
        LoadSubmissionCost cost{ read_load_operation_cost(costs.at("before_sync")),  read_load_operation_cost(costs.at("start_record")),
                                 read_load_operation_cost(costs.at("wait_event")),   read_load_operation_cost(costs.at("first_copy")),
                                 read_load_operation_cost(costs.at("copy")),         read_load_operation_cost(costs.at("first_layer_record")),
                                 read_load_operation_cost(costs.at("layer_record")), read_load_operation_cost(costs.at("tail")) };
        // An identity for a target-only resource, not a zero-cost executable op.
        const auto stream = graph.add_synthetic_node({
            .name = "unused target load stream",
            .is_cpu = false,
            .lane_key = "hicache.generated_load:" + key.first + ":" + key.second,
            .observed_point = core::DagObservedPoint{ key.first, key.second, 0, rank }
        });
        graph.mutable_node(stream).active = false;
        generated_submissions_.emplace(key, GeneratedSubmission{ *main, resources->worker, resources->compute, stream, model_layers.size(), cost });
    }
}

} // namespace markov::trace_graph::modules::hicache::runtime
