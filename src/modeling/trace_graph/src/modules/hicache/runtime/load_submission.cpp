#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/control_calibration.hpp"
#include "markov/trace_graph/modules/hicache/runtime/loads.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {
namespace {

constexpr auto cost_fields = { &LoadIndexOperationCost::main_us,
                               &LoadIndexOperationCost::main_residual_us,
                               &LoadIndexOperationCost::worker_us,
                               &LoadIndexOperationCost::dispatch_us,
                               &LoadIndexOperationCost::device_us };
constexpr auto submission_fields = { &LoadSubmissionCost::before_sync,  &LoadSubmissionCost::start_record, &LoadSubmissionCost::wait_event,
                                     &LoadSubmissionCost::first_copy,   &LoadSubmissionCost::copy,         &LoadSubmissionCost::first_layer_record,
                                     &LoadSubmissionCost::layer_record, &LoadSubmissionCost::tail };

LoadIndexOperationCost mean_cost(std::span<const LoadIndexOperationCost> samples) {
    LoadIndexOperationCost result;
    for (const auto field : cost_fields) {
        uint64_t total = 0;
        for (const auto & sample : samples) total = core::checked_add_u64(total, sample.*field, "Load operation cost overflow");
        result.*field = total / samples.size() + (total % samples.size() >= (samples.size() + 1) / 2);
    }
    return result;
}

} // namespace

std::optional<LoadSubmissionCost> observe_load_submission_cost(const HiCacheWriteExpansion & plan, size_t layers) {
    if (!plan.event_waits.empty() || plan.waits.size() != 1 || layers < 2) return std::nullopt;
    const auto & nodes = plan.nodes;
    std::vector<size_t> records, waits, copies;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto & node = nodes[i];
        if (!node.work.is_cpu || node.queue_member) continue;
        if (node.work.name.ends_with("Enqueue@record_event")) records.push_back(i);
        if (node.work.name.ends_with("Enqueue@wait_event")) waits.push_back(i);
        if (node.work.name.ends_with("AscendCL@aclrtMemcpy2dAsync")) copies.push_back(i);
    }
    const auto sync = plan.waits.front().consumer;
    if (waits.size() != 1 || records.size() != layers + 1 || copies.empty() || copies.size() % 2) return std::nullopt;
    if (!(sync < records[0] && records[0] < waits[0] && waits[0] < copies.front() && copies.back() < records[1])) return std::nullopt;

    size_t previous = 0;
    bool complete = true;
    const auto segment = [&](size_t end, bool submitted = false) {
        LoadIndexOperationCost cost;
        for (size_t i = previous; i <= end; ++i) {
            if (!nodes[i].work.is_cpu || nodes[i].queue_member) continue;
            cost.main_us = core::checked_add_u64(cost.main_us, nodes[i].work.duration, "Load CPU cost overflow");
            cost.main_residual_us = core::checked_add_u64(cost.main_residual_us, nodes[i].work.cpu_gap_after, "Load residual cost overflow");
        }
        previous = end + 1;
        if (!submitted) return cost;

        std::optional<size_t> job, device;
        for (size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].submission == end) {
                if (job) complete = false;
                job = i;
            }
        if (!job || !nodes[*job].queue_member) {
            complete = false;
            return cost;
        }
        for (const auto & edge : plan.edges)
            if (edge.from == *job && !nodes[edge.to].work.is_cpu) {
                if (device && *device != edge.to) complete = false;
                device = edge.to;
            }
        if (!device) {
            complete = false;
            return cost;
        }
        cost.worker_us = nodes[*job].work.duration;
        cost.dispatch_us = nodes[*job].work.cpu_task_ready_delay_us.value();
        cost.device_us = nodes[*device].work.duration;
        return cost;
    };
    LoadSubmissionCost cost;
    cost.before_sync = segment(sync);
    cost.start_record = segment(records[0], true);
    cost.wait_event = segment(waits[0], true);
    cost.first_copy = segment(copies.front());
    std::vector<LoadIndexOperationCost> repeated;
    for (size_t i = 1; i < copies.size(); ++i) repeated.push_back(segment(copies[i]));
    cost.copy = mean_cost(repeated);
    cost.first_layer_record = segment(records[1], true);
    repeated.clear();
    for (size_t i = 2; i < records.size(); ++i) repeated.push_back(segment(records[i], true));
    cost.layer_record = mean_cost(repeated);
    cost.tail = segment(plan.host_return);
    return complete ? std::optional{ cost } : std::nullopt;
}

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
    std::map<std::pair<std::string, std::string>, std::vector<LoadSubmissionCost>> base_costs;
    for (const auto & load : loads_) {
        if (load.layers.empty()) continue;
        const auto & record = load.record;
        const auto host = observe_host_template(source, record.pid, record.tid, record.source_start_us, record.source_end_us);
        const auto plan = prepare_write_expansion(source, queues, record, host, load.layers.back().source_record);
        if (const auto cost = observe_load_submission_cost(plan, load.layers.size())) base_costs[{ record.pid, record.tid }].push_back(*cost);
    }
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
        LoadSubmissionCost cost;
        const auto observed = base_costs.find(key);
        // The source supplies costs, never the number or order of target copies.
        // Shared measurements remain the fallback when its layout cannot be partitioned.
        if (observed != base_costs.end()) {
            for (const auto field : submission_fields) {
                std::vector<LoadIndexOperationCost> samples;
                for (const auto & sample : observed->second) samples.push_back(sample.*field);
                cost.*field = mean_cost(samples);
            }
        }
        else {
            if (!row) continue;
            const auto & costs = row->at("costs");
            cost = { read_load_operation_cost(costs.at("before_sync")),  read_load_operation_cost(costs.at("start_record")),
                     read_load_operation_cost(costs.at("wait_event")),   read_load_operation_cost(costs.at("first_copy")),
                     read_load_operation_cost(costs.at("copy")),         read_load_operation_cost(costs.at("first_layer_record")),
                     read_load_operation_cost(costs.at("layer_record")), read_load_operation_cost(costs.at("tail")) };
        }
        // An identity for a target-only resource, not a zero-cost executable op.
        const auto stream = graph.add_synthetic_node({
            .name = "unused target load stream",
            .is_cpu = false,
            .lane_key = "hicache.generated_load:" + key.first + ":" + key.second,
            .observed_point = core::DagObservedPoint{ key.first, key.second, 0, rank }
        });
        graph.mutable_node(stream).active = false;
        generated_submissions_.emplace(
            key,
            GeneratedSubmission{ *main, resources->worker, resources->compute, stream, model_layers.size(), cost, observed != base_costs.end() });
    }
}

} // namespace markov::trace_graph::modules::hicache::runtime
