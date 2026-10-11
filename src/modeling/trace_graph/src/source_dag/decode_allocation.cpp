#include "markov/trace_graph/source_dag/decode_allocation.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/model/replay.hpp"
#include "markov/trace_graph/structure/phase_shape.hpp"

#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace markov::trace_graph::source_dag {

using modules::hicache::HiCacheBatchPathEntry;
using modules::hicache::HiCacheFact;
using modules::hicache::HiCacheFactRole;
using modules::hicache::HiCachePhaseObservationAudit;
using modules::hicache::model::HiCacheReplayFact;

ObservedDecodeContexts observe_decode_contexts(const core::DagGraph & graph, const HiCachePhaseObservationAudit & phases) {
    using RequestKey = std::pair<std::string, std::string>;
    std::map<RequestKey, const modules::hicache::HiCachePhaseObservation *> requests_by_rank;
    std::set<RequestKey> ambiguous;
    for (const auto & source : phases.observations)
        if (source.request_ids.size() == 1) {
            const RequestKey key{ source.pid, source.request_ids.front() };
            if (!requests_by_rank.emplace(key, &source).second) ambiguous.insert(key);
        }

    ObservedDecodeContexts contexts;
    for (const auto & event : graph.runtime_observations()) {
        if (event.name != "runtime.hicache.decode_allocation" || event.arg("status") != "returned" || event.arg("token_per_req") != "1") continue;

        const auto requests = nlohmann::json::parse(event.arg("requests"));
        if (requests.size() != 1) continue;

        const auto & request = requests.front();
        const RequestKey key{ event.pid, request.at("request_id").get<std::string>() };
        const auto found = requests_by_rank.find(key);
        if (found == requests_by_rank.end() || event.ts < found->second->prefill_start_us) continue;

        const auto iteration = request.at("decode_batch_idx").get<uint64_t>();
        if (iteration >= found->second->decode_iteration_count) {
            ambiguous.insert(key);
            continue;
        }
        const auto expected = core::checked_add_u64(found->second->prompt_token_count, iteration, "Observed decode KV length overflow");
        if (request.at("kv_committed_len") != expected || request.at("kv_allocated_len") != expected) {
            ambiguous.insert(key);
            continue;
        }

        if (!contexts[key].emplace(iteration, structure::decode_context_tokens(found->second->prompt_token_count, iteration)).second) ambiguous.insert(key);
    }
    std::erase_if(contexts, [&](const auto & entry) {
        return ambiguous.contains(entry.first) || entry.second.size() != requests_by_rank.at(entry.first)->decode_iteration_count;
    });
    return contexts;
}

DecodeOperatorContexts decode_operator_contexts(const HiCachePhaseObservationAudit & phases, const ObservedDecodeContexts & contexts) {
    DecodeOperatorContexts result;
    for (const auto & phase : phases.observations) {
        if (phase.request_ids.size() != 1) continue;
        const auto found = contexts.find({ phase.pid, phase.request_ids.front() });
        if (found == contexts.end()) continue;

        for (const auto & [node, iteration] : phase.decode_node_iterations)
            result.emplace(node, DecodeOperationContext{ phase.pid, phase.request_ids.front(), iteration, found->second.at(iteration) });
    }
    return result;
}

std::vector<HiCacheReplayFact> observe_decode_allocations(const core::DagGraph & graph, std::span<const HiCacheReplayFact> facts,
                                                          const HiCachePhaseObservationAudit & phases, uint64_t begin, uint64_t end) {
    using Json = nlohmann::json;
    using Key = std::pair<std::string, std::string>; // pid, request id
    struct Origin {
        const HiCacheFact * fact = nullptr;
        const HiCacheBatchPathEntry * path = nullptr;
        uint64_t iterations = 0, observed = 0;
    };
    std::map<Key, Origin> inputs;
    for (const auto & phase : phases.observations)
        for (const auto & request : phase.request_ids)
            if (!inputs.emplace(Key{ phase.pid, request }, Origin{ .iterations = phase.decode_iteration_count }).second)
                throw std::runtime_error("Decode allocation requires an unambiguous request phase");
    size_t next_id = 0;
    for (const auto & item : facts) {
        if (item.role == HiCacheFactRole::CacheDecodeAllocation) throw std::logic_error("Decode observations already prepared");
        next_id = std::max(next_id, core::checked_add_u64(item.fact.source_node_id, 1, "Decode action identity overflow"));

        if (item.role != HiCacheFactRole::CacheExtendInput) continue;
        for (const auto & path : item.fact.batch_paths) {
            const auto found = inputs.find({ item.fact.pid, path.request_id });
            if (found == inputs.end()) continue;
            if (found->second.fact) throw std::runtime_error("Decode input has multiple cache-extend origins");
            found->second.fact = &item.fact;
            found->second.path = &path;
        }
    }
    std::vector<const core::TraceEvent *> allocations;
    std::map<Key, std::vector<const core::TraceEvent *>> forwards; // pid, tid
    for (const auto & event : graph.runtime_observations()) {
        if (event.ts < begin || event.ts >= end) continue;
        if (event.name == "runtime.hicache.decode_allocation") allocations.push_back(&event);
        if (event.name == "runtime.hicache.layer_waits" && event.arg("phase") == "DECODE") forwards[{ event.pid, event.tid }].push_back(&event);
    }
    std::ranges::sort(allocations, {}, [](const auto * event) { return std::tuple{ event->ts, event->pid, event->tid, event->index }; });
    for (auto & [lane, events] : forwards) std::ranges::sort(events, {}, [](const auto * event) { return event->ts; });
    std::set<const core::TraceEvent *> used_forwards;
    std::vector<HiCacheReplayFact> derived;
    for (const auto * event : allocations) {
        if (event->arg("status") != "returned" || event->arg("token_per_req") != "1")
            throw std::runtime_error("Decode allocation requires a successful non-speculative single-token call");

        const auto requests = Json::parse(event->arg("requests"));
        if (!requests.is_array() || requests.empty()) throw std::runtime_error("Decode allocation has no request identities");
        HiCacheFact action;
        std::vector<std::string> ids;
        for (const auto & request : requests) {
            const auto id = request.at("request_id").get<std::string>();
            const auto found = inputs.find({ event->pid, id });
            if (found == inputs.end() || !found->second.fact) throw std::runtime_error("Decode allocation lacks an approved workload input");

            auto & origin = found->second;
            if (origin.fact->cache_scope != event->arg("cache_scope") || origin.fact->tid != event->tid || event->ts <= origin.fact->source_ts)
                throw std::runtime_error("Decode allocation disagrees with its cache-extend origin");
            const auto iteration = request.at("decode_batch_idx").get<uint64_t>();
            const auto length = core::checked_add_u64(origin.path->token_count, iteration, "Observed decode length overflow");
            if (iteration != origin.observed || iteration >= origin.iterations || request.at("kv_committed_len") != length
                || request.at("kv_allocated_len") != length)
                throw std::runtime_error("Decode allocation has incomplete iteration or length observations");
            if (ids.empty()) {
                action = *origin.fact;
                action.batch_paths.clear();
            }
            auto path = *origin.path;
            path.position = action.batch_paths.size();
            action.batch_paths.push_back(std::move(path));
            action.decode_iterations.push_back(iteration);
            ids.push_back(id);
            ++origin.observed;
        }

        auto & lane = forwards[{ event->pid, event->tid }];
        const auto finish = core::checked_add_u64(event->ts, event->dur, "Decode allocation observation end overflow");
        const auto next = std::ranges::lower_bound(lane, finish, {}, [](const auto * forward) { return forward->ts; });
        if (next == lane.end() || (*next)->arg("status") != "returned" || Json::parse((*next)->arg("request_ids")) != Json(ids)
            || !used_forwards.insert(*next).second)
            throw std::runtime_error("Decode allocation has no unique subsequent forward");
        action.source_node_id = next_id;
        next_id = core::checked_add_u64(next_id, 1, "Decode action identity overflow");
        action.source_event_index = event->index;
        action.source_ts = action.ts = event->ts;
        action.dur = event->dur;
        action.event_name = event->name;
        action.role = "cache_decode_allocation";
        action.batch_kind = "decode";
        action.batch_size = action.batch_paths.size();
        action.execution_anchor_node_id.reset();
        derived.push_back({ std::move(action), HiCacheFactRole::CacheDecodeAllocation });
    }

    for (const auto & [key, origin] : inputs)
        if (!origin.fact || origin.observed != origin.iterations) throw std::runtime_error("Base profile lacks complete decode allocation observations");
    size_t forward_count = 0;
    for (const auto & [lane, events] : forwards) forward_count += events.size();
    if (used_forwards.size() != forward_count) throw std::runtime_error("Decode forwards are missing allocation boundaries");
    return derived;
}

} // namespace markov::trace_graph::source_dag
