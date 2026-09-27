#include "markov/trace_graph/modules/hicache/prefetch_worker_observation.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include <algorithm>
#include <set>

namespace markov::trace_graph::modules::hicache {
namespace {
uint64_t end(const core::TraceEvent & e) { return e.ts + e.dur; }
bool min_call(const core::TraceEvent & e, uint64_t numel) {
    return e.arg("status") == "returned" && e.arg("async_op") == "false" && e.arg("operation") == "all_reduce"
           && (e.arg("reduce_op") == "MIN" || e.arg("reduce_op") == "RedOpType.MIN") && e.arg_u64("numel") == numel;
}
} // namespace

PrefetchWorkerObservations observe_prefetch_workers(const patch::HiCacheSourceDagIndex & source, const CpuCollectiveObservation & collectives) {
    PrefetchWorkerObservations out;
    const auto & events = source.graph().runtime_observations();
    using Lane = std::pair<std::string, std::string>;
    std::map<Lane, std::vector<size_t>> lanes;
    std::map<std::pair<std::string, std::string>, std::vector<size_t>> enqueues;
    std::map<size_t, size_t> round_at;
    for (size_t r = 0; r < collectives.rounds.size(); ++r)
        for (const auto & call : collectives.rounds[r].calls) round_at.emplace(call.observation, r);
    for (size_t id = 0; id < events.size(); ++id) {
        const auto & e = events[id];
        if (e.name == "runtime.hicache.prefetch_enqueue") enqueues[{ e.pid, e.arg("operation_id") }].push_back(id);
        if (e.name == "runtime.hicache.prefetch_query" || e.name == "runtime.hicache.host_release" || e.name == "runtime.hicache.storage_drain"
            || (e.name == "runtime.cpu_collective" && (e.arg("role") == "prefetch_storage_hit_agreement" || e.arg("role") == "storage_control_drain")))
            lanes[{ e.pid, e.tid }].push_back(id);
    }
    const auto add_round = [&](size_t id, uint64_t numel, std::vector<size_t> & rounds, std::string & issue) {
        const auto found = round_at.find(id);
        if (!min_call(events[id], numel) || found == round_at.end() || !collectives.rounds[found->second].issue.empty()) {
            issue = "incomplete_or_invalid_min";
            return;
        }
        rounds.push_back(found->second);
    };
    for (auto & [lane, ids] : lanes) {
        std::ranges::stable_sort(ids, {}, [&](size_t id) { return events[id].ts; });
        std::optional<size_t> pending_query;
        std::vector<size_t> drain_calls;
        for (const auto id : ids) {
            const auto & e = events[id];
            if (e.name == "runtime.hicache.prefetch_query") {
                if (pending_query) out.queries[*pending_query].issue = "query_release_not_observed";
                pending_query = out.queries.size();
                out.queries.push_back({ .observation = id });
                auto & query = out.queries.back();
                const auto page = core::parse_exact_u64(e.arg("page_size")), hit = core::parse_exact_u64(e.arg("local_hit_tokens"));
                if (e.arg("status") != "returned" || e.arg("request_id").empty() || e.arg("operation_id").empty() || !page || !*page || !hit || *hit % *page)
                    query.issue = "invalid_query_result";
                const auto submit = enqueues.find({ e.pid, e.arg("operation_id") });
                if (submit == enqueues.end() || submit->second.size() != 1) query.issue = "missing_or_nonunique_enqueue";
                else {
                    query.enqueue = submit->second.front();
                    const auto & call = events[*query.enqueue];
                    // Queue.put is inside this envelope. Query can start before
                    // the caller returns, but cannot precede its entry.
                    if (call.arg("status") != "returned" || call.arg("request_id") != e.arg("request_id") || call.ts > e.ts)
                        query.issue = "enqueue_query_identity_or_order";
                    std::vector<size_t> candidates;
                    for (const auto & fact : source.fact_nodes())
                        if (fact.fact_role == "prefetch_candidate_anchor" && fact.phase == "end" && fact.pid == call.pid && fact.tid == call.tid
                            && fact.request_id == call.arg("request_id") && fact.timestamp_us <= call.ts && fact.timestamp_us + fact.duration_us >= end(call))
                            candidates.push_back(fact.node_id);
                    if (candidates.size() == 1) query.candidate_fact = candidates.front();
                    else query.issue = "missing_or_nonunique_enqueue_candidate";
                }
            }
            else if (e.name == "runtime.cpu_collective") {
                if (e.arg("role") == "storage_control_drain") drain_calls.push_back(id);
                else if (!pending_query) ++out.issues["query_min_without_query"];
                else {
                    auto & q = out.queries[*pending_query];
                    if (e.ts < end(events[q.observation])) q.issue = "query_min_before_query_return";
                    add_round(id, 1, q.agreement_rounds, q.issue);
                }
            }
            else if (e.name == "runtime.hicache.storage_drain") {
                StorageDrainObservation drain{ .observation = id };
                drain.revoked = core::parse_exact_u64(e.arg("n_revoke"));
                drain.backups = core::parse_exact_u64(e.arg("n_backup"));
                drain.released_pages = core::parse_exact_u64(e.arg("n_release"));
                drain.local_shutdown = e.arg("n_revoke") == "null" && e.arg("n_backup") == "null" && e.arg("n_release") == "null";
                const auto tp = core::parse_exact_u64(e.arg("tp_world_size"));
                if (e.arg("status") != "returned" || !tp || !*tp || (!drain.local_shutdown && (!drain.revoked || !drain.backups || !drain.released_pages)))
                    drain.issue = "invalid_drain_result";
                if (drain.local_shutdown && !drain_calls.empty()) drain.issue = "local_drain_after_unmatched_min";
                if (!drain.local_shutdown && tp && *tp > 1 && drain_calls.empty()) drain.issue = "drain_min_not_observed";
                std::set<std::string> groups;
                for (const auto call : drain_calls) {
                    add_round(call, 3, drain.agreement_rounds, drain.issue);
                    if (end(events[call]) > e.ts || !groups.insert(events[call].arg("group")).second) drain.issue = "ambiguous_drain_min_sequence";
                }
                drain_calls.clear();
                out.drains.push_back(std::move(drain));
            }
            else {
                HostReleaseObservation release{ .observation = id, .request_id = e.arg("request_id") };
                const auto count = core::parse_exact_u64(e.arg("token_count")), page = core::parse_exact_u64(e.arg("page_size"));
                if (e.arg("status") != "returned" || !count || !page || !*page || *count % *page) release.issue = "invalid_release_count";
                else release.pages = *count / *page;
                if (e.arg("role") == "prefetch_completion" || e.arg("role") == "prefetch_abort") release.owner = e.arg("role");
                else if (pending_query) {
                    auto & q = out.queries[*pending_query];
                    const auto & query = events[q.observation];
                    q.release = id;
                    release.query = pending_query;
                    release.owner = "query";
                    release.request_id = query.arg("request_id");
                    release.operation_id = query.arg("operation_id");
                    const auto expected = core::parse_exact_u64(query.arg("sync_group_count"));
                    if (!expected || q.agreement_rounds.size() != *expected) q.issue = "query_min_count_mismatch";
                    if (end(query) > e.ts) q.issue = "release_before_query_return";
                    for (const auto r : q.agreement_rounds)
                        for (const auto & call : collectives.rounds[r].calls)
                            if (events[call.observation].pid == e.pid && end(events[call.observation]) > e.ts) q.issue = "release_before_query_min";
                    pending_query.reset();
                }
                out.releases.push_back(std::move(release));
            }
        }
        if (pending_query) out.queries[*pending_query].issue = "query_release_not_observed";
        if (!drain_calls.empty()) ++out.issues["drain_min_without_return"];
    }
    std::map<size_t, size_t> worker_releases;
    std::map<Lane, std::vector<const patch::HiCacheSourceFactNode *>> worker_frames;
    for (const auto & fact : source.fact_nodes())
        if (fact.fact_role == "prefetch_io_observed") worker_frames[{ fact.pid, fact.tid }].push_back(&fact);
    for (auto & [lane, frames] : worker_frames) std::ranges::sort(frames, {}, [](const auto * fact) { return fact->timestamp_us; });
    for (auto & release : out.releases) {
        const auto & e = events[release.observation];
        if (release.owner == "query") {
            if (!out.queries[*release.query].issue.empty()) release.issue = "query_episode_incomplete";
            continue;
        }
        if (release.owner == "prefetch_abort") {
            if (release.request_id.empty()) release.issue = "abort_request_not_observed";
            continue; // Explicit scope, not a guessed worker or successful completion.
        }
        std::vector<const patch::HiCacheSourceFactNode *> parents;
        if (release.owner == "prefetch_completion") {
            for (const auto & fact : source.fact_nodes()) {
                if (fact.pid != e.pid || fact.tid != e.tid) continue;
                if (fact.fact_role == "prefetch_progress_observed" && fact.request_id == release.request_id && fact.timestamp_us <= e.ts
                    && fact.timestamp_us + fact.duration_us >= end(e))
                    parents.push_back(&fact);
            }
        }
        else {
            // Worker FIFO, not the nearest request on another thread. Each
            // completed _page_transfer has one release before the next begins.
            const auto & frames = worker_frames[{ e.pid, e.tid }];
            for (size_t i = 0; i < frames.size(); ++i) {
                const auto & fact = *frames[i];
                if (fact.timestamp_us + fact.duration_us <= e.ts && (i + 1 == frames.size() || frames[i + 1]->timestamp_us >= end(e))) parents.push_back(&fact);
            }
        }
        if (parents.size() != 1) release.issue = "missing_or_nonunique_release_parent";
        else {
            const auto & parent = *parents.front();
            release.parent_fact = parent.node_id;
            if (release.owner.empty()) {
                release.owner = "worker";
                release.request_id = parent.request_id;
                release.operation_id = parent.operation_id;
                ++worker_releases[parent.node_id];
                if (release.request_id.empty() || release.operation_id.empty()) release.issue = "worker_identity_not_observed";
                else if (!parent.completed_token_count_present || !release.pages || parent.source_page_size == 0
                         || parent.completed_token_count % parent.source_page_size
                         || parent.completed_token_count / parent.source_page_size + *release.pages != parent.page_hashes.size())
                    release.issue = "worker_release_page_count_mismatch";
            }
        }
    }
    for (auto & release : out.releases)
        if (release.owner == "worker" && worker_releases[*release.parent_fact] != 1) release.issue = "multiple_worker_releases";
    for (const auto & fact : source.fact_nodes())
        if (fact.fact_role == "prefetch_io_observed" && !worker_releases.contains(fact.node_id)) ++out.issues["worker_release_not_observed"];
    for (auto & query : out.queries) {
        if (!query.enqueue || !query.release) continue;
        const auto &q = events[query.observation], &submit = events[*query.enqueue], &release = events[*query.release];
        const auto allocated = core::parse_exact_u64(submit.arg("allocated_tokens")), released = core::parse_exact_u64(release.arg("token_count"));
        const auto page = core::parse_exact_u64(q.arg("page_size")), local = core::parse_exact_u64(q.arg("local_hit_tokens"));
        if (!allocated || !released || !page || !*page || !local || *allocated % *page || *released > *allocated
            || submit.arg("page_size") != q.arg("page_size") || release.arg("page_size") != q.arg("page_size")) {
            query.issue = "query_allocation_or_release_geometry";
            continue;
        }
        std::vector<const patch::HiCacheSourceFactNode *> workers;
        for (const auto & fact : source.fact_nodes())
            if (fact.fact_role == "prefetch_io_observed" && fact.pid == q.pid && fact.operation_id == q.arg("operation_id")) workers.push_back(&fact);
        const auto issued = *allocated - *released;
        if (issued > *local || (issued && workers.size() != 1) || (!issued && !workers.empty())) query.issue = "query_worker_not_observed_or_inconsistent";
        else if (issued
                 && (workers.front()->request_id != q.arg("request_id") || workers.front()->source_page_size != *page
                     || workers.front()->page_hashes.size() != issued / *page || workers.front()->timestamp_us < end(release)))
            query.issue = "query_worker_identity_or_order";
    }
    std::map<size_t, std::set<std::string>> round_requests;
    std::map<std::pair<std::string, std::string>, size_t> query_identities;
    for (const auto & q : out.queries) {
        const auto & event = events[q.observation];
        ++query_identities[{ event.pid, event.arg("operation_id") }];
        for (const auto r : q.agreement_rounds) round_requests[r].insert(event.arg("request_id"));
    }
    for (auto & q : out.queries) {
        const auto & event = events[q.observation];
        if (query_identities[{ event.pid, event.arg("operation_id") }] != 1) q.issue = "nonunique_query_operation";
        for (const auto r : q.agreement_rounds)
            if (round_requests[r].size() != 1) q.issue = "query_min_request_mismatch";
    }
    for (auto & release : out.releases)
        if (release.query && !out.queries[*release.query].issue.empty()) release.issue = "query_episode_incomplete";
    for (const auto & row : out.queries)
        if (!row.issue.empty()) ++out.issues[row.issue];
    for (const auto & row : out.releases)
        if (!row.issue.empty()) ++out.issues[row.issue];
    for (const auto & row : out.drains)
        if (!row.issue.empty()) ++out.issues[row.issue];
    return out;
}
} // namespace markov::trace_graph::modules::hicache
