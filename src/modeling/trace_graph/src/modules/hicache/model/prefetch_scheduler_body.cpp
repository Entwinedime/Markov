#include "markov/trace_graph/modules/hicache/model/prefetch_scheduler_body.hpp"
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::model {
PrefetchSchedulerExecution append_prefetch_scheduler_body(const PrefetchSchedulerTiming & body, const CpuRankNodes & entries, simulation::FutureDag & future) {
    if (body.steps.size() != PREFETCH_SCHEDULER_SEQUENCE.size() || body.tail.size() != entries.size())
        throw std::invalid_argument("Scheduler execution requires costs bound for every action and rank");
    PrefetchSchedulerExecution execution;
    const auto local = [&](int rank, size_t previous, const PrefetchSchedulerLocalTiming & timing) {
        const auto work =
            future.append({ .name = "hicache_scheduler_cpu", .category = "hicache_patch", .duration = timing.cpu_us, .cpu_gap_after = timing.gap_us },
                          simulation::BoundaryOrder::Ordinary,
                          entries.at(rank));
        const auto point =
            future.append({ .name = "hicache_scheduler_boundary", .category = "hicache_patch" }, simulation::BoundaryOrder::Ordinary, entries.at(rank));
        future.depend(previous, work);
        future.depend(work, point, core::DagEdgeKind::Sequential);
        return point;
    };
    auto current = entries;
    const auto action = [&](PrefetchSchedulerAction kind, int rank, size_t point, bool apply) {
        const auto gate =
            future.append({ .name = "hicache_scheduler_action_return", .category = "execution_gate" }, simulation::BoundaryOrder::Ordinary, entries.at(rank));
        future.depend(point, gate);
        execution.actions[point] = { kind, rank, apply, gate };
        return gate;
    };
    for (const auto kind : PREFETCH_SCHEDULER_SEQUENCE) {
        const auto & step = body.steps.at(kind);
        for (auto & [rank, node] : current) {
            node = local(rank, node, step.before.at(rank));
            node = action(kind, rank, node, false);
        }
        current = append_cpu_collective(step.communication, current, future);
        for (auto & [rank, node] : current) {
            // The write-confirmation owner expands the whole post-MIN tail,
            // including publication and asynchronous event destruction.
            // A fixed scheduler tail must not charge the same work again.
            if (kind != PrefetchSchedulerAction::WriteCompletion) node = local(rank, node, step.after.at(rank));
            node = action(kind, rank, node, true);
        }
    }
    for (const auto & [rank, node] : current) execution.returned[rank] = local(rank, node, body.tail.at(rank));
    return execution;
}

} // namespace markov::trace_graph::modules::hicache::model
