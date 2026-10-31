/** @file Source task identities shared by template observation and simulation.
 * Queue order is resolved during full simulation, never copied from a donor. */
#pragma once
#include "markov/trace_graph/core/dag_graph.hpp"
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace markov::trace_graph::simulation::detail {

struct CpuTask {
    size_t first, last, submission, queue;
    uint64_t ready_delay_us = 0;
    uint64_t submission_overlap_us = 0;
};

struct CpuTaskQueues {
    struct WorkerResource {
        size_t queue;
        int logical_input;
    };

    std::vector<CpuTask> tasks;
    std::vector<size_t> node_task, submitted_task;
    std::map<std::string, WorkerResource, std::less<>> worker_queues;
    size_t queue_count = 0;
    size_t source_program_tasks_reused = 0;
#ifdef DEBUG
    std::map<size_t, std::string> unavailable_lanes;
#endif

    [[nodiscard]] bool replaces(const core::DagEdge & edge) const;
    void materialize(core::DagGraph & graph, const std::vector<std::vector<size_t>> & order) const;
};

[[nodiscard]] CpuTaskQueues discover_cpu_task_queues(const core::DagGraph & graph);

} // namespace markov::trace_graph::simulation::detail
