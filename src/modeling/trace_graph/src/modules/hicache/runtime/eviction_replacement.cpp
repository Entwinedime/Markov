#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/host_removal.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_calls.hpp"
#include <algorithm>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

void HiCacheWriteCalls::replace_source_write_calls(core::DagGraph & graph) {
    // Eviction envelopes have already been replaced as a whole. Remaining
    // source submissions retain only their entry/return sites: target state
    // determines whether they write and how many pages/calls are necessary.
    std::vector<size_t> submissions;
    for (const auto & [node, actions] : actions_)
        for (const auto & [action, call] : actions)
            if (action == Action::Submit) submissions.push_back(call);
    if (submissions.empty()) return;

    const patch::HiCacheSourceDagIndex observed(graph);
    std::vector<HiCacheHostTemplate> hosts;
    std::vector<std::vector<size_t>> opening_nodes;
    hosts.reserve(submissions.size());
    opening_nodes.reserve(submissions.size());
    for (const auto id : submissions) {
        const auto & call = calls_[id];
        const auto begin_us = graph.event_for_node(call.entry_node).ts;
        const auto end_us = graph.event_for_node(call.return_node).ts;
        hosts.push_back(observe_host_template(observed, call.pid, call.tid, begin_us, end_us));
        opening_nodes.emplace_back();
        if (call.at_us >= begin_us) continue;

        // The suffix cannot own CPU leaves that were already clipped. It can
        // still own their queued work through exact original caller metadata.
        auto & initial = opening_nodes.back();
        initial = writes_.source_retained_nodes(call.source.value());
        for (const auto & node : graph.nodes()) {
            if (!node.active || !node.is_cpu) continue;
            const auto & event = graph.event_for_node(node.id);
            if (event.cat != "enqueue" || event.pid != call.pid || event.arg("pre_window_enqueue_tid") != call.tid) continue;

            const auto first = core::parse_u64(event.arg("pre_window_enqueue_start_us")).value();
            const auto last = core::parse_u64(event.arg("pre_window_enqueue_end_us")).value();
            if (first >= call.at_us && first < end_us && last <= end_us) initial.push_back(node.id);
        }
    }

    std::vector<HiCacheHostRegion> regions;
    for (size_t i = 0; i < submissions.size(); ++i) {
        const auto & call = calls_[submissions[i]];
        regions.push_back({ call.entry_node, call.return_node, &hosts[i], opening_nodes[i] });
    }
    (void)core::apply_dag_mutation_plan(graph, plan_host_removal(observed, regions));

    // Resolve against the surviving stream order, not the removed payload's
    // observed GPU timestamps. Generated host returns delay the original
    // continuation; asynchronous completion does not block it unnecessarily.
    const patch::HiCacheSourceDagIndex surviving(graph);
    for (const auto id : submissions) {
        auto & call = calls_[id];
        call.successors = { call.return_node };
        if (const auto resources = write_resources_.find({ call.pid, call.tid }); resources != write_resources_.end()) {
            for (const auto resource : { resources->second.compute, resources->second.transfer }) {
                const auto lane = graph.node(resource).lane_id;
                call.positions.emplace(lane, observe_write_stream_position(surviving, resource, call.pid, call.tid, call.at_us));
            }
        }
    }
}

void HiCacheWriteCalls::replace_source_evictions(core::DagGraph & graph, uint64_t begin, uint64_t end) {
    // Replace both policy directions as complete envelopes. The target state
    // chooses writes/releases; the successful allocation return is retained.
    std::vector<patch::HiCacheSourceFactNode> evictions;
    std::vector<core::TraceEvent> points;
    const patch::HiCacheSourceDagIndex observed(graph);
    for (const auto & fact : observed.fact_nodes()) {
        if (fact.fact_role != "capacity_result_observed" || fact.phase != "end" || !fact.duration_us || fact.timestamp_us < begin
            || fact.timestamp_us + fact.duration_us > end)
            continue;
        // Even an unchanged policy can select different victims, pages and
        // releases. The observed envelope locates execution; it does not
        // supply the target's operation count or host program.
        evictions.push_back(fact);
        for (const auto at : { fact.timestamp_us, fact.timestamp_us + fact.duration_us }) {
            core::TraceEvent point;
            point.name = "hicache.eviction_replacement";
            point.pid = fact.pid;
            point.tid = fact.tid;
            point.ts = at;
            points.push_back(std::move(point));
        }
    }
    if (evictions.empty()) return;
    const auto bound = bind_hicache_control_points(graph, points);
    const patch::HiCacheSourceDagIndex source(graph);
    std::vector<HiCacheHostTemplate> hosts;
    std::vector<Call> replacements;
    hosts.reserve(evictions.size());
    for (size_t i = 0; i < evictions.size(); ++i) {
        const auto & fact = evictions[i];
        if (!bound[2 * i] || !bound[2 * i + 1]) throw std::runtime_error("Eviction replacement lacks exact entry/exit gates");
        const auto owner = observe_write_call_owner(source, replay_.facts(), fact.pid, fact.tid, fact.timestamp_us, fact.timestamp_us + fact.duration_us);
        Call call{ .owner = owner,
                   .return_node = *bound[2 * i + 1],
                   .pid = fact.pid,
                   .tid = fact.tid,
                   .at_us = fact.timestamp_us,
                   .entry_node = *bound[2 * i],
                   .successors = { *bound[2 * i + 1] },
                   .lane = std::string(graph.node_lane_key(*bound[2 * i])),
                   .retained_allocation_return = true };
        bool returned = false;
        for (const auto & guard : capacity_guards_)
            if (guard.owner == owner && guard.begin <= fact.timestamp_us && guard.end >= fact.timestamp_us + fact.duration_us) {
                call.return_node = guard.return_node;
                returned = true;
            }
        if (!returned) {
            for (const auto & attempt : load_attempts_)
                if (attempt.owner == owner && attempt.event.arg("allocated") == "true" && attempt.event.ts >= fact.timestamp_us + fact.duration_us) {
                    call.return_node = attempt.return_node;
                    returned = true;
                }
        }
        if (!returned) throw std::runtime_error("Eviction replacement has no enclosing allocation return");
        hosts.push_back(observe_host_template(source, fact.pid, fact.tid, fact.timestamp_us, fact.timestamp_us + fact.duration_us));
        replacements.push_back(std::move(call));
    }
    std::vector<HiCacheHostRegion> regions;
    for (size_t i = 0; i < hosts.size(); ++i) regions.push_back({ *bound[2 * i], *bound[2 * i + 1], &hosts[i] });
    std::set<size_t> obsolete_calls;
    for (size_t i = 0; i < calls_.size(); ++i) {
        const auto & call = calls_[i];
        for (const auto & eviction : evictions) {
            if (call.source) {
                // Ownership is a property of the observed call, not of a
                // reusable cost template. Boundary/lifecycle calls can remain
                // in the graph without supplying a complete formal template.
                const auto * observed_call = source.fact_node(*call.source);
                if (!observed_call) throw std::logic_error("Source write call lost its timing fact during eviction replacement");
                const auto & envelope = observe_write_envelope(source,
                                                               observed_call->pid,
                                                               observed_call->tid,
                                                               observed_call->timestamp_us,
                                                               observed_call->timestamp_us + observed_call->duration_us);
                if (envelope.pid == eviction.pid && envelope.tid == eviction.tid && envelope.timestamp_us >= eviction.timestamp_us
                    && envelope.timestamp_us + envelope.duration_us <= eviction.timestamp_us + eviction.duration_us)
                    obsolete_calls.insert(i);
            }
            else if (call.pid == eviction.pid && call.tid == eviction.tid && call.at_us >= eviction.timestamp_us
                     && call.at_us < eviction.timestamp_us + eviction.duration_us)
                obsolete_calls.insert(i);
        }
    }
    const auto plan = plan_host_removal(source, regions);
    (void)core::apply_dag_mutation_plan(graph, plan);
    for (auto & [node, actions] : actions_) std::erase_if(actions, [&](const auto & action) { return obsolete_calls.contains(action.second); });
    std::erase_if(actions_, [&](const auto & entry) { return entry.second.empty() || !graph.node(entry.first).active; });
    source_owners_.clear();
    wait_owners_.clear();
    for (const auto & [node, actions] : actions_)
        for (const auto & [action, index] : actions) {
            if (action == Action::Submit) source_owners_.insert(calls_[index].owner);
            if (action == Action::Wait) wait_owners_.insert(calls_[index].owner);
        }
    const patch::HiCacheSourceDagIndex surviving(graph);
    for (auto & call : replacements) {
        for (const auto & [id, donor] : observed_write_fragments_)
            if (donor.pid == call.pid && donor.tid == call.tid) bind_call_resources(surviving, call, donor.resources);
        for (const auto & donor : observed_eviction_fragments_)
            if (donor.pid == call.pid && donor.tid == call.tid) bind_call_resources(surviving, call, donor.resources);
        if (!graph.node(call.return_node).active) throw std::runtime_error("Eviction replacement removed its allocation return");
        allocation_returns_[call.return_node].push_back(call.owner);
        capacity_at_.emplace(call.entry_node, call.owner);
        if (!capacity_calls_.emplace(call.owner, std::move(call)).second)
            throw std::runtime_error("Multiple source evictions share one target allocation action");
    }
}

} // namespace markov::trace_graph::modules::hicache::runtime
