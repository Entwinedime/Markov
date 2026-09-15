/** @file Source task identities; queue order is resolved during full simulation. */
#pragma once
#include "markov/trace_graph/core/dag_graph.hpp"
#include <vector>

namespace markov::trace_graph::simulation::detail {

struct CpuTask {
    size_t first, last, submission, queue;
    uint64_t ready_delay_us = 0;
    uint64_t submission_overlap_us = 0;
};

struct CpuTaskQueues {
    std::vector<CpuTask> tasks;
    std::vector<size_t> node_task, submitted_task;
    size_t queue_count = 0;

    [[nodiscard]] bool replaces(const core::DagEdge& edge) const;
    void materialize(core::DagGraph& graph, const std::vector<std::vector<size_t>>& order) const;
};

[[nodiscard]] CpuTaskQueues discover_cpu_task_queues(const core::DagGraph& graph);

} // namespace markov::trace_graph::simulation::detail
