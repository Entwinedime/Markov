#include "markov/trace_graph/source_dag/confirmation.hpp"

#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/source_dag/call_regions.hpp"
#include "markov/trace_graph/source_dag/tensor_cost.hpp"
#include "markov/trace_graph/structure/hicache/confirmation.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <optional>
#include <utility>
#include <vector>

namespace markov::trace_graph::source_dag {

void append_write_confirmation_observations(const modules::hicache::patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                            size_t observation, uint64_t begin_us, uint64_t confirmed_batches, bool corrected, nlohmann::json & rows) {
    const auto & graph = source.graph();
    const auto & outer = graph.runtime_observations().at(observation);
    const bool blocking = outer.arg("blocking") == "true";
    const auto end = core::checked_add_u64(outer.ts, outer.dur, "Write confirmation end overflow");
    const auto region = observe_submitted_call_region(source, queues, observation);
    const auto tasks = observe_queued_call_tasks(source, queues, region);
    if (!tasks) return;

    std::vector<QueuedCallTask> cleanup;
    for (const auto & task : *tasks) {
        const auto & submit = graph.event_for_node(queues.tasks.at(task.task).submission);
        if (submit.ts < begin_us) continue;
        if (submit.name != "Enqueue@destroy_event" || !task.devices.empty()) return;
        cleanup.push_back(task);
    }
    if (cleanup.size() != 2 * confirmed_batches) return;

    std::vector<const core::TraceEvent *> acks, waits;
    std::vector<structure::hicache::WriteConfirmationBatch> batches;
    for (const auto & ack : graph.runtime_observations()) {
        if (ack.name != "runtime.hicache.write_ack" || ack.pid != outer.pid || ack.tid != outer.tid || ack.ts < begin_us || ack.ts + ack.dur > end) continue;
        if (ack.arg("status") != "returned" || ack.arg("release_lock") != (blocking ? "false" : "true") || ack.arg("storage_enabled") != "true") return;
        acks.push_back(&ack);
    }
    if (acks.size() != confirmed_batches) return;
    std::ranges::sort(acks, {}, [](const auto * ack) { return ack->ts; });
    const auto tail = source.call_interval_ownership(outer.pid, outer.tid, begin_us, end - begin_us);
    if (tail.status != "ready" || tail.has_node_overlap) return;
    for (const auto node : tail.owned_node_ids)
        if (core::synchronizes_recorded_event(graph.event_for_node(node))) waits.push_back(&graph.event_for_node(node));
    if (waits.size() != confirmed_batches) return;
    std::ranges::sort(waits, {}, [](const auto * wait) { return wait->ts; });

    struct Interval {
        uint64_t begin, end;
        std::optional<size_t> submission;
    };
    std::vector<Interval> intervals{
        { begin_us, waits.front()->ts, std::nullopt }
    };
    auto cursor = begin_us;
    const auto append_release = [&](size_t event) {
        const auto submission = queues.tasks.at(cleanup[event].task).submission;
        const auto & submit = graph.event_for_node(submission);
        const auto submitted = core::checked_add_u64(submit.ts, submit.dur, "Event destruction submission end overflow");
        if (submit.ts < cursor || submitted > end) return false;

        intervals.push_back({ cursor, submitted, submission });
        cursor = submitted;
        return true;
    };
    for (size_t batch = 0; batch < acks.size(); ++batch) {
        const auto & ack = *acks[batch];
        const auto tokens = core::parse_u64(ack.arg("backup_tokens"));
        const auto published = core::parse_u64(ack.arg("published_nodes"));
        if (!tokens || !published || waits[batch]->ts < cursor || waits[batch]->ts + waits[batch]->dur > ack.ts) return;
        batches.push_back({ *tokens, *published });
        if (batch) intervals.push_back({ cursor, waits[batch]->ts, std::nullopt });
        cursor = ack.ts + ack.dur;
        intervals.push_back({ waits[batch]->ts, cursor, std::nullopt });
        if (!blocking)
            for (size_t event = 0; event < 2; ++event)
                if (!append_release(2 * batch + event)) return;
    }
    if (blocking)
        for (size_t event = 0; event < cleanup.size(); ++event)
            if (!append_release(event)) return;
    intervals.push_back({ cursor, end, std::nullopt });

    const auto program = structure::hicache::write_confirmation_program(batches, blocking);
    for (size_t i = 0; i < program.size(); ++i) {
        const auto & interval = intervals[i];
        (void)append_cpu_interval_observations(source, outer, program[i].work, interval.begin, interval.end, corrected, rows, interval.submission);
    }
    for (const auto & task : cleanup)
        for (auto & row : queued_task_observations(source, queues, task, structure::hicache::event_destruction_work(), corrected))
            rows.push_back(std::move(row));
}

} // namespace markov::trace_graph::source_dag
