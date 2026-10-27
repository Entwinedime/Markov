#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/modules/hicache/runtime/host_removal.hpp"
#include "markov/trace_graph/modules/hicache/runtime/loads.hpp"
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

void HiCacheLoads::replace_source_submissions(core::DagGraph & graph) {
    const adapters::sglang::HiCacheSourceIndex source(graph);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    std::vector<HiCacheHostTemplate> hosts;
    hosts.reserve(loads_.size());
    std::vector<HiCacheHostExpansion> empty_costs(loads_.size());
    for (size_t i = 0; i < loads_.size(); ++i) {
        const auto & record = loads_[i].record;
        if (!replacement_boundaries_.at(2 * i) || !replacement_boundaries_.at(2 * i + 1))
            throw std::runtime_error("Generated load replacement lacks exact call boundaries");
        hosts.push_back(observe_host_template(source, record.pid, record.tid, record.source_start_us, record.source_end_us));
        if (!loads_[i].layers.empty()) continue;
        auto empty = prepare_host_expansion(source, queues, hosts.back(), "original empty load: ");
        if (!hosts.back().worker_nodes.empty() || !empty.streams.empty() || !empty.waits.empty() || !empty.event_waits.empty())
            throw std::runtime_error("Empty load cost contains device or worker work");
        // Only the exact unchanged call migrates its source cost. Another
        // call on the same thread is not interchangeable cost evidence.
        empty_costs[i] = std::move(empty);
    }
    std::vector<HiCacheHostRegion> regions;
    for (size_t i = 0; i < loads_.size(); ++i) regions.push_back({ *replacement_boundaries_[2 * i], *replacement_boundaries_[2 * i + 1], &hosts[i] });

    // Consumers must already be detached from original layer records. Replace
    // all submissions together; no source copy count survives as target work.
    const auto removal = plan_host_removal(source, queues, regions);
    (void)core::apply_dag_mutation_plan(graph, removal);
    const adapters::sglang::HiCacheSourceIndex retained(graph);
    submit_at_.clear();
    for (size_t i = 0; i < loads_.size(); ++i) {
        auto & load = loads_[i];
        const auto & record = load.record;
        Insertion insertion{ std::move(empty_costs[i]), *replacement_boundaries_[2 * i + 1], {} };
        const auto found = generated_submissions_.find({ record.pid, record.tid });
        if (found != generated_submissions_.end()) {
            const auto & program = found->second;
            insertion.positions.emplace(graph.node(program.compute).lane_id,
                                        observe_write_stream_position(retained, program.compute, record.pid, record.tid, record.source_start_us));
            insertion.positions.emplace(graph.node(program.stream).lane_id, HiCacheWriteStreamPosition{});
        }
        insertions_.emplace(i, std::move(insertion));
        if (!submit_at_.emplace(*replacement_boundaries_[2 * i], i).second) throw std::runtime_error("Target loads share a submission gate");
        load.layers.clear();
    }
}

void HiCacheLoads::rebind_retained_resources(const adapters::sglang::HiCacheSourceIndex & source, const std::map<size_t, size_t> & members) {
    const auto & graph = source.graph();
    for (auto & [index, insertion] : insertions_) {
        const auto & record = loads_[index].record;
        rebind_host_stream_positions(source, insertion.positions, record.pid, record.tid, record.source_start_us);
    }

    for (auto & [key, program] : generated_submissions_) program.worker = members.at(graph.node(program.worker).lane_id);
}

} // namespace markov::trace_graph::modules::hicache::runtime
