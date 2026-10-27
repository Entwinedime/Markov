#include "markov/trace_graph/structure/hicache/query.hpp"
#include "markov/trace_graph/structure/cpu_collective.hpp"
#include "markov/trace_graph/structure/hicache/scheduler.hpp"

namespace markov::trace_graph::structure::hicache {

OperationWork prefetch_scheduler_operation(std::string_view role, std::string_view scope, int rank, size_t participants) {
    const bool collective = role.ends_with(".collective");
    const bool receive = role == "receive.collective";
    OperationWork work{
        .component = "hicache_direct",
        .operation = "prefetch.scheduler." + std::string(role),
        .implementation = collective ? (receive ? "gloo.broadcast.int64" : "gloo.all_reduce.int32") : "sglang.serial_scheduler",
        .timing_scope = std::string(scope),
        .features = { { "rank", static_cast<double>(rank) }, { "participants", static_cast<double>(participants) } }
    };
    if (collective) work.features["message_bytes"] = receive ? 8.0 : (role == "storage_drain.collective" ? 12.0 : 4.0);
    return work;
}

OperationWork prefetch_check_collective(bool completion, int rank, size_t participants, std::string_view timing_scope) {
    return {
        .component = "hicache_direct",
        .operation = completion ? "prefetch.collective.completion_min" : "prefetch.collective.state_max",
        .implementation = "gloo.all_reduce.int32",
        .timing_scope = std::string(timing_scope),
        .features = { { "rank", static_cast<double>(rank) },
                     { "participants", static_cast<double>(participants) },
                     { "message_bytes", completion ? 4.0 : 8.0 } }
    };
}

OperationWork prefetch_check_interval(std::string_view role, int rank, size_t participants) {
    return {
        .component = "hicache_direct",
        .operation = "prefetch.check." + std::string(role),
        .implementation = "sglang.prefetch_check",
        .timing_scope = "control_interval",
        .features = { { "rank", static_cast<double>(rank) }, { "participants", static_cast<double>(participants) } }
    };
}

std::map<int, PrefetchQueryRankWork> prefetch_query_work(std::span<const int> participants) {
    std::map<int, PrefetchQueryRankWork> result;
    const auto dependencies = cpu_collective_entries(CpuCollectiveKind::AllReduce, participants);
    for (const auto rank : participants) {
        const auto interval = [&](const char * scope) {
            return OperationWork{
                .component = "hicache_direct",
                .operation = "prefetch.query",
                .implementation = "sglang.storage_hit_min",
                .timing_scope = scope,
                .features = { { "rank", static_cast<double>(rank) }, { "participants", static_cast<double>(participants.size()) } }
            };
        };

        result.emplace(rank,
                       PrefetchQueryRankWork{ .before_sample = interval("before_sample_interval"),
                                              .sample_to_min = interval("sample_to_min_interval"),
                                              .min_to_apply = interval("min_to_apply_interval"),
                                              .agreement = interval("all_reduce_envelope_remainder"),
                                              .agreement_entries = dependencies.at(rank) });
    }
    return result;
}

} // namespace markov::trace_graph::structure::hicache
