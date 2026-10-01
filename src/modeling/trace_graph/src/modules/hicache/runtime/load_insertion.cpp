#include "markov/trace_graph/modules/hicache/runtime/host_removal.hpp"
#include "markov/trace_graph/modules/hicache/runtime/loads.hpp"
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
        Donor donor{ record.pid, record.tid, prepare_write_expansion(source, queues, record, host, load.layers.back().source_record), {}, {} };
        for (auto & node : donor.plan.nodes) {
            constexpr std::string_view prefix = "target write: ";
            if (node.work.name.starts_with(prefix)) node.work.name.replace(0, prefix.size(), "target load: ");
        }
        for (const auto & layer : load.layers) donor.records.push_back(donor.plan.source_nodes.at(layer.source_record));
        donor.lanes = donor.plan.resource_lanes(graph);
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
        for (const auto & donor : donors_) {
            if (donor.pid != record.pid || donor.tid != record.tid) continue;
            const auto observe = [&](size_t node) {
                const auto lane = graph.node(node).lane_id;
                if (!insertion.positions.contains(lane))
                    insertion.positions.emplace(lane, observe_write_stream_position(source, node, record.pid, record.tid, record.source_start_us));
            };
            for (const auto node : donor.plan.resource_nodes()) observe(node);
        }
        insertions_.emplace(i, std::move(insertion));
        empty_hosts.emplace(i, std::move(host));
    }
    core::DagMutationPlan replacement{ .component = "hicache_load_submission" };
    std::map<size_t, uint64_t> remaining_gaps;
    for (const auto & [i, host] : empty_hosts) {
        const auto & record = loads_[i].record;
        const auto first = insertions_.at(i).continuation;
        (void)append_host_entry_gate(source,
                                     first,
                                     { record.pid, record.tid, record.source_start_us, graph.node(first).gpu_id },
                                     "load_entry:" + std::to_string(i),
                                     "load call entry",
                                     replacement);
        for (const auto node : host.main.owned_node_ids) replacement.set_node_durations.push_back({ node, 0 });
        for (const auto & gap : host.main.owned_gap_slices) {
            auto & remaining = remaining_gaps.try_emplace(gap.owner_node_id, graph.node(gap.owner_node_id).cpu_gap_after).first->second;
            const auto owned = gap.owned_end_us - gap.owned_start_us;
            if (remaining < owned) throw std::runtime_error("Empty load gap ownership exceeds its retained interval");
            remaining -= owned;
        }
    }
    for (const auto & [node, duration] : remaining_gaps) replacement.set_cpu_gaps.push_back({ node, duration });

    // Publish callback bindings only after the entire prospective replacement
    // is accepted. A missing boundary must not leave a half-rewired source DAG.
    const auto applied = core::apply_dag_mutation_plan(graph, replacement);
    std::erase_if(submit_at_, [&](const auto & entry) { return empty_hosts.contains(entry.second); });
    for (const auto & [i, host] : empty_hosts) submit_at_.emplace(applied.synthetic_node_ids.at("load_entry:" + std::to_string(i)), i);
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
        if (!replacement_boundaries_.at(2 * i) || !replacement_boundaries_.at(2 * i + 1))
            throw std::runtime_error("Generated load replacement lacks exact call boundaries");
        hosts.push_back(observe_host_template(source, record.pid, record.tid, record.source_start_us, record.source_end_us));
        if (!loads_[i].layers.empty()) continue;
        auto empty = prepare_host_expansion(source, queues, hosts.back(), "target empty load: ");
        if (!hosts.back().worker_nodes.empty() || !empty.streams.empty() || !empty.waits.empty() || !empty.event_waits.empty())
            throw std::runtime_error("Empty load cost contains device or worker work");
        empty_costs.try_emplace(std::pair{ record.pid, record.tid }, std::move(empty));
    }
    std::vector<HiCacheHostRegion> regions;
    for (size_t i = 0; i < loads_.size(); ++i) regions.push_back({ *replacement_boundaries_[2 * i], *replacement_boundaries_[2 * i + 1], &hosts[i] });
    // No source submission survives to impose its number of copies or CPU
    // launches on target work. Surviving consumers were detached by layer_calls.
    const auto removal = plan_host_removal(source, regions);
    (void)core::apply_dag_mutation_plan(graph, removal);
    const patch::HiCacheSourceDagIndex retained(graph);
    submit_at_.clear();
    for (size_t i = 0; i < loads_.size(); ++i) {
        auto & load = loads_[i];
        const auto & record = load.record;
        const auto key = std::pair{ record.pid, record.tid };
        const auto found = generated_submissions_.find(key);
        if (found == generated_submissions_.end()) throw std::runtime_error("Target load replacement lacks operation costs or proven scheduler resources");
        const auto & program = found->second;
        HiCacheHostExpansion empty;
        if (const auto at = empty_costs.find(key); at != empty_costs.end()) empty = at->second;
        else {
            // The source never returned on an empty queue. Reuse the measured
            // independent bookkeeping cost, explicitly as a CPU-cost proxy.
            std::pair<double, double> remainder{};
            empty = generated_cpu_control(graph.node_lane_key(*replacement_boundaries_[2 * i]),
                                          program.cost.tail.main_us,
                                          program.cost.tail.main_residual_us,
                                          remainder,
                                          "target empty load: independent bookkeeping cost proxy");
        }
        Insertion insertion{ std::move(empty), *replacement_boundaries_[2 * i + 1], {} };
        insertion.positions.emplace(graph.node(program.compute).lane_id,
                                    observe_write_stream_position(retained, program.compute, record.pid, record.tid, record.source_start_us));
        insertion.positions.emplace(graph.node(program.stream).lane_id, HiCacheWriteStreamPosition{});
        insertions_.emplace(i, std::move(insertion));
        if (!submit_at_.emplace(*replacement_boundaries_[2 * i], i).second) throw std::runtime_error("Target loads share a submission gate");
        load.layers.clear();
    }
}

void HiCacheLoads::rebind_retained_resources(const patch::HiCacheSourceDagIndex & source, const std::map<size_t, size_t> & members) {
    const auto & graph = source.graph();
    for (auto & [index, insertion] : insertions_) {
        const auto & record = loads_[index].record;
        rebind_host_stream_positions(source, insertion.positions, record.pid, record.tid, record.source_start_us);
    }

    std::vector<HiCacheHostExpansion *> plans;
    for (auto & donor : donors_) plans.push_back(&donor.plan);
    rebind_host_worker_queues(graph, members, plans);

    for (auto & [key, program] : generated_submissions_) {
        const auto lane = graph.node(program.worker).lane_id;
        program.worker = members.at(lane);
    }
}

} // namespace markov::trace_graph::modules::hicache::runtime
