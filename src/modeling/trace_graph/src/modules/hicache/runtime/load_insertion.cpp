#include "markov/trace_graph/modules/hicache/runtime/loads.hpp"
#include "markov/trace_graph/modules/hicache/runtime/host_removal.hpp"
#include <algorithm>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

void HiCacheLoads::prepare_insertions(core::DagGraph & graph, std::span<const std::optional<size_t>> boundaries) {
    if (!submission_calibration_.empty()) {
        replacement_boundaries_.assign(boundaries.begin(), boundaries.end());
        return;
    }
    const patch::HiCacheSourceDagIndex source(graph);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    for (const auto & load : loads_) {
        if (load.layers.empty()) continue;
        const auto & record = load.record;
        const auto host = observe_host_template(source, record.pid, record.tid, record.source_start_us, record.source_end_us);
        Donor donor{ record.pid, record.tid,
                     prepare_write_expansion(source, queues, record, host, load.layers.back().source_record), {}, {} };
        for (auto & node : donor.plan.nodes) {
            constexpr std::string_view prefix = "target write: ";
            if (node.work.name.starts_with(prefix)) node.work.name.replace(0, prefix.size(), "target load: ");
        }
        for (const auto & layer : load.layers) donor.records.push_back(donor.plan.source_nodes.at(layer.source_record));
        for (const auto & stream : donor.plan.streams) donor.lanes.push_back(graph.node(stream.source_node).lane_id);
        for (const auto & wait : donor.plan.waits) donor.lanes.push_back(graph.node(wait.source_node).lane_id);
        donors_.push_back(std::move(donor));
    }
    // Observe all empty calls before changing their original work. Their CPU
    // identities remain as zero-cost boundaries; only the owned work is replaced.
    std::map<size_t, HiCacheHostTemplate> empty_hosts;
    for (size_t i = 0; i < loads_.size(); ++i) {
        const auto & record = loads_[i].record;
        if (!loads_[i].layers.empty()) continue;
        if (!boundaries[2 * i] || !boundaries[2 * i + 1]) throw std::runtime_error("Empty load lacks exact call boundaries");
        auto host = observe_host_template(source, record.pid, record.tid, record.source_start_us, record.source_end_us);
        auto empty = prepare_host_expansion(source, queues, host, "empty load: ");
        if (!host.worker_nodes.empty() || !empty.streams.empty() || !empty.waits.empty() || !empty.event_waits.empty())
            throw std::runtime_error("Empty load contains device or worker work");
        Insertion insertion{ std::move(empty), *boundaries[2 * i], {} };
        if (const auto at = generated_submissions_.find({record.pid, record.tid}); at != generated_submissions_.end()) {
            const auto & program = at->second;
            insertion.positions.emplace(graph.node(program.compute).lane_id,
                observe_write_stream_position(source, program.compute, record.pid, record.tid, record.source_start_us));
            insertion.positions.emplace(graph.node(program.stream).lane_id, HiCacheWriteStreamPosition{});
        }
        for (const auto & donor : donors_) {
            if (donor.pid != record.pid || donor.tid != record.tid) continue;
            const auto observe = [&](size_t node) {
                const auto lane = graph.node(node).lane_id;
                if (!insertion.positions.contains(lane))
                    insertion.positions.emplace(lane, observe_write_stream_position(source, node, record.pid, record.tid, record.source_start_us));
            };
            for (const auto & stream : donor.plan.streams) observe(stream.source_node);
            for (const auto & wait : donor.plan.waits) observe(wait.source_node);
        }
        insertions_.emplace(i, std::move(insertion));
        empty_hosts.emplace(i, std::move(host));
    }
    for (const auto & [i, host] : empty_hosts) {
        const auto & record = loads_[i].record;
        const auto first = insertions_.at(i).continuation;
        const auto gate = graph.add_synthetic_node({ .name = "load call entry", .category = "execution_gate",
            .lane_key = std::string(graph.node_lane_key(first)),
            .observed_point = core::DagObservedPoint{ record.pid, record.tid, record.source_start_us, graph.node(first).gpu_id } });
        for (const auto edge : source.incoming_edge_ids(first))
            if (graph.edge(edge).active && graph.edge(edge).dst == first) graph.mutable_edge(edge).dst = gate;
        graph.add_edge(gate, first, core::DagEdgeKind::Sequential);
        for (auto at = submit_at_.begin(); at != submit_at_.end(); ++at)
            if (at->second == i) { submit_at_.erase(at); break; }
        submit_at_.emplace(gate, i);
        for (const auto node : host.main.owned_node_ids) graph.set_node_duration(node, 0);
        for (const auto & gap : host.main.owned_gap_slices) {
            auto & remaining = graph.mutable_node(gap.owner_node_id).cpu_gap_after;
            const auto owned = gap.owned_end_us - gap.owned_start_us;
            if (remaining < owned) throw std::runtime_error("Empty load gap ownership exceeds its retained interval");
            remaining -= owned;
        }
    }
}

void HiCacheLoads::replace_source_submissions(core::DagGraph & graph) {
    if (submission_calibration_.empty()) return;
    const patch::HiCacheSourceDagIndex source(graph);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    std::vector<HiCacheHostTemplate> hosts;
    hosts.reserve(loads_.size());
    std::map<std::pair<std::string, std::string>, HiCacheHostExpansion> empty_costs;
    for (size_t i = 0; i < loads_.size(); ++i) {
        const auto & record = loads_[i].record;
        if (!replacement_boundaries_.at(2*i) || !replacement_boundaries_.at(2*i+1))
            throw std::runtime_error("Generated load replacement lacks exact call boundaries");
        hosts.push_back(observe_host_template(source, record.pid, record.tid, record.source_start_us, record.source_end_us));
        if (!loads_[i].layers.empty()) continue;
        auto empty = prepare_host_expansion(source, queues, hosts.back(), "target empty load: ");
        if (!hosts.back().worker_nodes.empty() || !empty.streams.empty() || !empty.waits.empty() || !empty.event_waits.empty())
            throw std::runtime_error("Empty load cost contains device or worker work");
        empty_costs.try_emplace(std::pair{record.pid, record.tid}, std::move(empty));
    }
    std::vector<HiCacheHostRegion> regions;
    for (size_t i = 0; i < loads_.size(); ++i)
        regions.push_back({*replacement_boundaries_[2*i], *replacement_boundaries_[2*i+1], &hosts[i]});
    // No source submission survives to impose its number of copies or CPU
    // launches on target work. Surviving consumers were detached by layer_calls.
    const auto removal = plan_host_removal(source, regions);
    (void)core::apply_dag_mutation_plan(graph, removal);
    const patch::HiCacheSourceDagIndex retained(graph);
    submit_at_.clear();
    for (size_t i = 0; i < loads_.size(); ++i) {
        auto & load = loads_[i];
        const auto & record = load.record;
        const auto key = std::pair{record.pid, record.tid};
        const auto found = generated_submissions_.find(key);
        if (found == generated_submissions_.end())
            throw std::runtime_error("Target load replacement lacks operation costs or proven scheduler resources");
        const auto & program = found->second;
        HiCacheHostExpansion empty;
        if (const auto at = empty_costs.find(key); at != empty_costs.end()) empty = at->second;
        else {
            // The source never returned on an empty queue. Reuse the measured
            // independent bookkeeping cost, explicitly as a CPU-cost proxy.
            std::pair<double, double> remainder{};
            empty = generated_cpu_control(graph.node_lane_key(*replacement_boundaries_[2*i]),
                program.cost.tail.main_us, program.cost.tail.main_residual_us, remainder,
                "target empty load: independent bookkeeping cost proxy");
        }
        Insertion insertion{std::move(empty), *replacement_boundaries_[2*i+1], {}};
        insertion.positions.emplace(graph.node(program.compute).lane_id,
            observe_write_stream_position(retained, program.compute, record.pid, record.tid, record.source_start_us));
        insertion.positions.emplace(graph.node(program.stream).lane_id, HiCacheWriteStreamPosition{});
        insertions_.emplace(i, std::move(insertion));
        if (!submit_at_.emplace(*replacement_boundaries_[2*i], i).second)
            throw std::runtime_error("Target loads share a submission gate");
        load.layers.clear();
    }
}

void HiCacheLoads::rebind_workers(const core::DagGraph & graph) {
    std::vector<HiCacheHostExpansion *> plans;
    for (auto & donor : donors_) plans.push_back(&donor.plan);
    rebind_host_worker_queues(graph, plans);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    for (auto & [key, program] : generated_submissions_) {
        const auto lane = graph.node(program.worker).lane_id;
        const auto at = std::ranges::find_if(queues.tasks, [&](const auto & task) { return graph.node(task.first).lane_id == lane; });
        if (at == queues.tasks.end()) throw std::runtime_error("Generated load lost its proven worker FIFO");
        program.worker = at->first;
    }
}

} // namespace markov::trace_graph::modules::hicache::runtime
