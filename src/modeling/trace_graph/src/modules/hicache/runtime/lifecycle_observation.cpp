#include "markov/trace_graph/modules/hicache/runtime/lifecycle_observation.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include <algorithm>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {
namespace {
uint64_t start_ns(const core::TraceEvent & event) { return event.ts * 1000 + event.ts_submicro_ns; }
uint64_t end_ns(const core::TraceEvent & event) { return start_ns(event) + event.dur * 1000 + event.dur_submicro_ns; }
bool contains(const core::TraceEvent & outer, const core::TraceEvent & inner) {
    return outer.pid == inner.pid && outer.tid == inner.tid && start_ns(outer) <= start_ns(inner) && end_ns(inner) <= end_ns(outer);
}
uint64_t count(const core::TraceEvent & event, const char * key) {
    const auto value = core::parse_u64(event.arg(key));
    if (!value) throw std::runtime_error(std::string("Lifecycle observation lacks token count: ") + key);
    return *value;
}
} // namespace

std::vector<HiCacheLifecycleStep> lifecycle_ready_steps(const HiCacheLifecycleInsert & insert) {
    std::vector<HiCacheLifecycleStep> steps;
    for (const auto & event : insert.node_boundaries) {
        const auto first = count(event, "path_begin_tokens"), last = count(event, "path_end_tokens");
        if (event.name == "runtime.hicache.node_publish") {
            const auto check = std::ranges::find_if(insert.node_boundaries, [&](const auto & other) {
                return other.name == "runtime.hicache.write_policy_check" && other.arg("node_id") == event.arg("node_id")
                       && count(other, "path_begin_tokens") == first && count(other, "path_end_tokens") == last;
            });
            if (check != insert.node_boundaries.end()) {
                if (end_ns(event) > start_ns(*check)) throw std::runtime_error("New leaf policy precedes its publication return");
                continue;
            }
        }
        auto ready = event;
        const auto at = event.name == "runtime.hicache.write_policy_check" ? start_ns(event) : end_ns(event);
        ready.ts = at / 1000;
        ready.ts_submicro_ns = at % 1000;
        ready.dur = ready.dur_submicro_ns = 0;
        ready.name += ".ready";
        steps.push_back({first, last, std::move(ready)});
    }
    std::ranges::sort(steps, {}, &HiCacheLifecycleStep::begin_tokens);
    uint64_t covered = 0, previous = start_ns(insert.envelope);
    for (const auto & step : steps) {
        if (step.begin_tokens != covered || step.end_tokens <= covered || step.end_tokens > insert.path_tokens)
            throw std::runtime_error("Lifecycle milestones do not uniquely cover the input path");
        if (start_ns(step.ready) < previous || start_ns(step.ready) > end_ns(insert.envelope))
            throw std::runtime_error("Lifecycle milestones do not follow path execution order");
        covered = step.end_tokens;
        previous = start_ns(step.ready);
    }
    if (covered != insert.path_tokens) throw std::runtime_error("Lifecycle milestones leave an unobserved path suffix");
    return steps;
}

std::vector<HiCacheLifecycleInsert> observe_lifecycle_inserts(
    const core::DagGraph & graph, std::span<const model::HiCacheReplayFact> facts, uint64_t begin, uint64_t end) {
    std::vector<HiCacheLifecycleInsert> result;
    const auto in_window = [&](const core::TraceEvent & event) {
        return start_ns(event) >= begin * 1000 && start_ns(event) <= end * 1000;
    };
    for (const auto & event : graph.runtime_observations()) {
        const bool retained_tail = event.arg("formal_window_context") == "causal_tail" && start_ns(event) > end * 1000;
        if (event.name != "runtime.hicache.radix_insert" || (!in_window(event) && !retained_tail)) continue;
        if (event.arg("status") != "returned") throw std::runtime_error("Radix insert observation did not return");
        const core::TraceEvent * enclosing = nullptr;
        for (const auto * collection : {&graph.hicache_fact_events(), &graph.tail_context_events()})
        for (const auto & raw : *collection) {
            if (raw.arg("phase") != "end" || !contains(raw, event)
                || parse_hicache_fact_metadata(raw).role != "cache_lifecycle_commit") continue;
            if (enclosing) throw std::runtime_error("Radix insert has multiple enclosing lifecycles");
            enclosing = &raw;
        }
        if (!enclosing) throw std::runtime_error("Radix insert has no enclosing lifecycle");
        const model::HiCacheReplayFact * owner = nullptr;
        for (const auto & item : facts) {
            if (item.role != HiCacheFactRole::CacheLifecycleCommit || item.fact.source_event_index != enclosing->index
                || item.fact.pid != enclosing->pid || item.fact.tid != enclosing->tid
                || item.fact.source_ts != hicache_fact_boundary_timestamp(*enclosing)) continue;
            if (owner) throw std::runtime_error("Radix insert has multiple replay owners");
            owner = &item;
        }
        if (!owner) throw std::runtime_error("Radix insert lifecycle is not a replay input");
        const auto chunked = event.arg("chunked");
        if (chunked != "true" && chunked != "false") throw std::runtime_error("Radix insert lacks chunked state");
        if (owner->fact.chunked && *owner->fact.chunked != (chunked == "true"))
            throw std::runtime_error("Radix insert and lifecycle disagree on chunked state");
        result.push_back({ owner->fact.source_node_id, event, count(event, "path_tokens"), {} });
    }
    for (const auto & event : graph.runtime_observations()) {
        if ((event.name != "runtime.hicache.write_policy_check" && event.name != "runtime.hicache.node_publish"
             && event.name != "runtime.hicache.device_restore")) continue;
        HiCacheLifecycleInsert * owner = nullptr;
        for (auto & insert : result) {
            if (!contains(insert.envelope, event)) continue;
            if (owner) throw std::runtime_error("Node boundary has multiple enclosing inserts");
            owner = &insert;
        }
        if (!owner) {
            if (!in_window(event)) continue;
            // A formal-window cut may retain an inner event but not its whole
            // insertion. Do not turn that partial call into a new target site.
            const bool partial = std::ranges::any_of(graph.runtime_observations(), [&](const auto & outer) {
                return outer.name == "runtime.hicache.radix_insert" && !in_window(outer) && contains(outer, event);
            });
            if (partial) continue;
            throw std::runtime_error("Node boundary has no enclosing insert");
        }
        const auto first = count(event, "path_begin_tokens"), last = count(event, "path_end_tokens");
        if (first >= last || last > owner->path_tokens || event.arg("node_id").empty() || event.arg("status") != "returned")
            throw std::runtime_error("Invalid node boundary within radix insert");
        if (event.arg("chunked") != owner->envelope.arg("chunked")) throw std::runtime_error("Node boundary and insert disagree on chunked state");
        owner->node_boundaries.push_back(event);
    }
    for (auto & insert : result)
        std::ranges::stable_sort(insert.node_boundaries, {}, start_ns);
    return result;
}

} // namespace markov::trace_graph::modules::hicache::runtime
