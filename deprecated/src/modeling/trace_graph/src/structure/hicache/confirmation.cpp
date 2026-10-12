#include "markov/trace_graph/structure/hicache/confirmation.hpp"

#include <string>
#include <utility>

namespace markov::trace_graph::structure::hicache {

OperationWork event_destruction_work() {
    return { "hicache_io_control", "runtime.event_destroy", "torch_npu.queue:destroy_event", "worker_service", { { "event_count", 1.0 } } };
}

std::vector<WriteConfirmationStep> write_confirmation_program(std::span<const WriteConfirmationBatch> batches, bool blocking) {
    if (blocking) {
        std::vector<WriteConfirmationStep> steps;
        steps.push_back({
            { "hicache_io_control", "write.completion_entry", batches.empty() ? "blocking_empty" : "blocking", "cpu_service", {} }
        });
        for (size_t batch = 0; batch < batches.size(); ++batch) {
            if (batch)
                steps.push_back({
                    { "hicache_io_control", "write.completion_advance", "blocking", "cpu_service", {} }
                });
            const auto & input = batches[batch];
            steps.push_back({
                { "hicache_io_control",
                 "write.completion_poll", "blocking_storage",
                 "cpu_service", { { "backup_tokens", static_cast<double>(input.backup_tokens) }, { "published_nodes", static_cast<double>(input.published_nodes) } } },
                false,
                batch
            });
        }

        // The queue keeps every batch alive throughout the blocking loop.
        // clear() releases earlier batches; the final two loop-local events
        // are released on return. All destruction is after the final ACK and
        // submits CPU-only work without waiting for its worker completion.
        for (size_t event = 0; event < 2 * batches.size(); ++event) {
            OperationWork work{ "hicache_io_control",
                                event ? "write.completion.release_next" : "write.completion.release_first",
                                "blocking",
                                "cpu_service",
                                {} };
            if (!event)
                work.features = {
                    { "confirmed_operations", static_cast<double>(batches.size()) }
                };
            steps.push_back({ std::move(work), true });
        }
        if (!batches.empty())
            steps.push_back({
                { "hicache_io_control", "write.completion.return", "blocking", "cpu_service", {} }
            });
        return steps;
    }

    if (batches.empty()) return { { { "hicache_io_control", "write.confirmation_tail", "nonblocking", "cpu_service", { { "confirmed_operations", 0.0 } } } } };

    std::vector<WriteConfirmationStep> steps;
    const auto append = [&](std::string name, bool destroy = false, std::optional<size_t> batch = std::nullopt) {
        OperationWork work{ "hicache_io_control", "write.confirmation." + name, "nonblocking_storage", "cpu_service", {} };
        if (batch) {
            const auto & input = batches[*batch];
            work.features = {
                {   "backup_tokens",   static_cast<double>(input.backup_tokens) },
                { "published_nodes", static_cast<double>(input.published_nodes) }
            };
        }
        steps.push_back({ std::move(work), destroy, batch });
    };

    append("entry");
    for (size_t batch = 0; batch < batches.size(); ++batch) {
        if (batch) append("advance");
        append("ack", false, batch);
        append("release_first", true);
        append("release_second", true);
    }
    append("return");
    return steps;
}

} // namespace markov::trace_graph::structure::hicache
