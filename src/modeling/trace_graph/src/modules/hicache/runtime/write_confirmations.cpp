#include "markov/trace_graph/modules/hicache/runtime/write_confirmations.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/control_calibration.hpp"
#include "markov/trace_graph/modules/hicache/runtime/host_removal.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_calls.hpp"
#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {
namespace {
core::TraceEvent boundary(const std::string & pid, const std::string & tid, uint64_t at) {
    core::TraceEvent point;
    point.name = "write_confirmation_boundary";
    point.pid = pid;
    point.tid = tid;
    point.ts = at;
    return point;
}
} // namespace

void HiCacheWriteConfirmations::prepare(core::DagGraph & graph, const std::string & calibration, uint64_t begin, uint64_t end,
                                        const std::vector<core::TraceEvent> & replaced, std::optional<uint64_t> idle_since_us) {
    std::ifstream file(calibration);
    if (!file) throw std::runtime_error("Cannot read independent write confirmation calibration");
    const auto document = nlohmann::json::parse(file);
    if (document.at("role") != "fixed_calibration" || document.at("source_manifest").get<std::string>().empty())
        throw std::runtime_error("Write confirmations need independent calibration provenance");
    const auto observations = graph.runtime_observations();
    std::vector<Tail> calls;
    std::vector<core::TraceEvent> points;
    for (const auto & event : observations) {
        if (event.name != "runtime.hicache.write_completion" || event.arg("status") != "returned" || event.ts > end
            || (event.ts < begin && (!idle_since_us || event.ts < *idle_since_us)))
            continue;
        if (std::ranges::any_of(replaced, [&](const auto & span) {
                return event.pid == span.pid && event.tid == span.tid && span.ts <= event.ts && span.ts + span.dur >= event.ts + event.dur;
            }))
            continue;
        const bool blocking = event.arg("blocking") == "true";
        if (blocking && (event.ts < begin || event.ts + event.dur > end)) continue;
        uint64_t start = event.ts;
        if (!blocking) {
            const core::TraceEvent * reduction = nullptr;
            for (const auto & item : observations)
                if (item.name == "runtime.cpu_collective" && item.arg("role") == "write_completion_check" && item.pid == event.pid && item.tid == event.tid
                    && item.ts >= event.ts && item.ts + item.dur <= event.ts + event.dur) {
                    if (reduction) throw std::runtime_error("Write confirmation has multiple MIN calls");
                    reduction = &item;
                }
            if (!reduction) throw std::runtime_error("Write confirmation has no MIN return");
            // Match queue confirmation admission: retain the full application
            // tail of a complete in-window MIN, without moving the HTTP cut.
            if (reduction->ts < begin || reduction->ts + reduction->dur > end) continue;
            start = reduction->ts + reduction->dur;
            tails_.push_back({ event.pid, event.tid, start, event.ts + event.dur });
        }
        calls.push_back({ event.pid, event.tid, start, event.ts + event.dur });
        points.push_back(boundary(event.pid, event.tid, start));
        points.push_back(boundary(event.pid, event.tid, event.ts + event.dur));
    }
    const auto bound = bind_hicache_control_points(graph, points);
    const patch::HiCacheSourceDagIndex source(graph);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    std::map<Lane, std::map<std::string, size_t>> resources;
    for (size_t i = 0; i < calls.size(); ++i) {
        const auto & call = calls[i];
        if (!bound[2 * i] || !bound[2 * i + 1]) throw std::runtime_error("Write confirmation lacks exact boundaries");
        const auto host = observe_host_template(source, call.pid, call.tid, call.begin, call.end);
        const auto plan = prepare_host_expansion(source, queues, host);
        auto & roles = resources[{ call.pid, call.tid }];
        for (const auto & [role, node] : host_resource_roles(graph, plan, *bound[2 * i])) {
            const auto [at, inserted] = roles.emplace(role, node);
            if (!inserted && graph.node_lane_key(at->second) != graph.node_lane_key(node))
                throw std::runtime_error("Confirmation role maps to different base resources");
        }
    }
    for (auto & [lane, roles] : resources) {
        const auto rank = graph.node(roles.at("main")).gpu_id;
        std::map<uint64_t, std::vector<const nlohmann::json *>> groups;
        for (const auto & row : document.at("rows")) {
            if (row.at("status") != "ready" || row.at("rank").get<int>() != rank) continue;
            const auto operations = row.at("confirmed_operations").get<uint64_t>();
            if (row.at("confirmed_batches").get<uint64_t>() != operations || row.at("template").at("event_waits").size() != operations)
                throw std::runtime_error("Confirmation calibration needs explicit one-operation batches");
            groups[operations].push_back(&row);
        }
        for (auto & [operations, samples] : groups) {
            std::ranges::stable_sort(samples, {}, [](const auto * row) {
                return row->at("main_cpu_us").template get<uint64_t>() + row->at("main_gap_us").template get<uint64_t>();
            });
            const auto & selected = samples[samples.size() / 2]->at("template");
            for (const auto & node : selected.at("nodes")) {
                const auto role = node.at("resource").get<std::string>();
                if (roles.contains(role) || !role.starts_with("worker|")) continue;
                if (const auto worker = find_host_worker_resource(graph, queues, roles.at("main"), role.substr(7))) roles.emplace(role, *worker);
            }
            templates_[lane].emplace(operations, import_host_template(selected, graph, queues, roles));
        }
        if (!templates_[lane].contains(0)) throw std::runtime_error("Confirmation calibration lacks an empty tail");
    }
}

void HiCacheWriteConfirmations::replace_source_tails(core::DagGraph & graph) {
    std::vector<core::TraceEvent> points;
    for (const auto & tail : tails_) {
        points.push_back(boundary(tail.pid, tail.tid, tail.begin));
        points.push_back(boundary(tail.pid, tail.tid, tail.end));
    }
    const auto bound = bind_hicache_control_points(graph, points);
    const patch::HiCacheSourceDagIndex source(graph);
    std::vector<HiCacheHostTemplate> hosts;
    hosts.reserve(tails_.size());
    for (const auto & tail : tails_) hosts.push_back(observe_host_template(source, tail.pid, tail.tid, tail.begin, tail.end));
    std::vector<HiCacheHostRegion> regions;
    for (size_t i = 0; i < hosts.size(); ++i) {
        if (!bound[2 * i] || !bound[2 * i + 1]) throw std::runtime_error("Confirmation replacement lost its boundaries");
        regions.push_back({ *bound[2 * i], *bound[2 * i + 1], &hosts[i] });
    }
    (void)core::apply_dag_mutation_plan(graph, plan_host_removal(source, regions));
}

void HiCacheWriteConfirmations::rebind_workers(const core::DagGraph & graph, const std::map<size_t, size_t> & members) {
    std::vector<HiCacheHostExpansion *> plans;
    for (auto & [lane, samples] : templates_)
        for (auto & [count, plan] : samples) plans.push_back(&plan);
    rebind_host_worker_queues(graph, members, plans);
}

size_t expand_write_confirmation_batches(const HiCacheHostExpansion & empty, const HiCacheHostExpansion & single, std::span<const size_t> completions,
                                         std::optional<size_t> anchor, simulation::FutureDag & future) {
    if (!empty.event_waits.empty() || !empty.streams.empty() || !empty.waits.empty())
        throw std::invalid_argument("Empty write confirmation must not depend on a device event");
    if (completions.empty()) return expand_host(empty, {}, future, anchor).host_return;
    if (single.event_waits.size() != 1 || !single.streams.empty() || !single.waits.empty())
        throw std::invalid_argument("Write confirmation primitive requires exactly one completed batch event");
    auto last = anchor;
    for (const auto event : completions) last = expand_host(single, {}, future, last, std::span(&event, 1)).host_return;
    return *last;
}

size_t HiCacheWriteConfirmations::expand(const WriteConfirmationWork & work, model::HiCacheState & state, const HiCacheWriteCalls & writes,
                                         simulation::FutureDag & future) const {
    const auto & samples = templates_.at({ work.fact.pid, work.fact.tid });
    const auto pending = state.unacknowledged_device_writes(work.fact);
    if (pending.size() < work.confirmed_writes) throw std::logic_error("Sampled confirmation prefix is no longer pending");
    std::vector<size_t> events;
    for (size_t i = 0; i < work.confirmed_writes; ++i) events.push_back(writes.completion_for(pending[i]));
    if (!events.empty() && !samples.contains(1)) throw std::runtime_error("Independent confirmation calibration lacks a single-batch CPU cost");
    return expand_write_confirmation_batches(samples.at(0), samples.at(events.empty() ? 0 : 1), events, work.fact.execution_anchor_node_id, future);
}
} // namespace markov::trace_graph::modules::hicache::runtime
