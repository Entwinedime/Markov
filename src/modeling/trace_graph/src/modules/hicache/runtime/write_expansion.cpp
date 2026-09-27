#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include <algorithm>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

HiCacheHostExpansion generated_cpu_control(std::string_view lane, double cpu_us, double residual_us, std::pair<double, double> & remainders,
                                           std::string_view name) {
    const auto cpu = core::truncate_to_u64(cpu_us + remainders.first);
    const auto residual = core::truncate_to_u64(residual_us + remainders.second);
    if (cpu_us < 0 || residual_us < 0 || !cpu || !residual) throw std::invalid_argument("Generated CPU control requires finite nonnegative cost estimates");
    remainders.first += cpu_us - *cpu;
    remainders.second += residual_us - *residual;
    HiCacheHostExpansion plan;
    plan.nodes.push_back({
        { .name = std::string(name), .category = "hicache_patch", .lane_key = std::string(lane), .duration = *cpu, .cpu_gap_after = *residual }
    });
    plan.nodes.push_back({
        { .name = "target control return", .lane_key = std::string(lane) }
    });
    plan.edges.push_back({ 0, 1, core::DagEdgeKind::Sequential });
    plan.host_return = 1;
    return plan;
}

HiCacheHostExpansion generated_completion_check(std::string_view lane, size_t events, double entry_cpu_us, double entry_residual_us, double event_cpu_us,
                                                double event_residual_us, std::pair<double, double> & remainders) {
    auto plan = generated_cpu_control(lane, entry_cpu_us, entry_residual_us, remainders, "target write check entry");
    for (size_t i = 0; i < events; ++i) {
        auto body = generated_cpu_control(lane, event_cpu_us, event_residual_us, remainders, "target write acknowledgement: base control proxy");
        const auto offset = plan.nodes.size();
        plan.edges.push_back({ plan.host_return, offset, core::DagEdgeKind::Sequential });
        plan.nodes.insert(plan.nodes.end(), body.nodes.begin(), body.nodes.end());
        for (const auto & edge : body.edges) plan.edges.push_back({ offset + edge.from, offset + edge.to, edge.kind });
        plan.event_waits.push_back({ core::DagNode::kNoNode, offset });
        plan.host_return = offset + body.host_return;
    }
    return plan;
}


HiCacheHostExpansion calibrated_cpu_control(std::string_view lane, double cost_us, double & remainder_us, std::string_view name) {
    const auto total = cost_us + remainder_us;
    const auto duration = core::truncate_to_u64(total);
    if (!(cost_us > 0.0) || !duration) throw std::invalid_argument("Calibrated control requires a finite positive CPU cost");
    remainder_us = total - *duration;
    HiCacheHostExpansion plan;
    plan.nodes.push_back({
        { .name = std::string(name), .category = "hicache_patch", .lane_key = std::string(lane), .duration = *duration }
    });
    return plan;
}

std::map<size_t, size_t> observe_host_worker_members(const core::DagGraph & graph) {
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    std::map<size_t, size_t> members;
    for (const auto & task : queues.tasks) members.emplace(graph.node(task.first).lane_id, task.first);
    return members;
}

void rebind_host_worker_queues(const core::DagGraph & graph, const std::map<size_t, size_t> & members,
                              std::span<HiCacheHostExpansion *> plans) {
    std::vector<std::pair<HiCacheHostExpansion::Node *, size_t>> bindings;
    for (auto * plan : plans)
        for (auto & node : plan->nodes) {
            if (!node.queue_member) continue;
            const auto member = members.find(graph.node(*node.queue_member).lane_id);
            if (member == members.end()) throw std::runtime_error("Host template has no surviving task on its original worker queue");
            bindings.emplace_back(&node, member->second);
        }
    for (const auto & [node, member] : bindings) node->queue_member = member;
}

HiCacheHostSequence::HiCacheHostSequence(const std::map<size_t, HiCacheWriteStreamPosition> & positions, HiCacheWriteStreamInsertions & insertions,
                                         simulation::FutureDag & future, std::optional<size_t> previous_host_return)
    : original_(positions),
      insertions_(insertions),
      future_(future),
      host_return_(previous_host_return) {}

std::vector<HiCacheWriteStreamPosition> HiCacheHostSequence::resolve(std::span<const size_t> lanes) const {
    std::vector<HiCacheWriteStreamPosition> result;
    // The shared insertion ledger owns stream tails across host sequences.
    // Resolve only the lanes this operation actually uses.
    for (const auto lane : lanes) result.push_back(insertions_.position(lane, original_.at(lane)));
    return result;
}

void HiCacheHostSequence::advance(std::span<const size_t> lanes, std::span<const size_t> tails, size_t host_return) {
    host_return_ = host_return;
    for (size_t i = 0; i < tails.size(); ++i) {
        insertions_.advance(lanes[i], original_.at(lanes[i]), tails[i]);
        insertions_.constrain_synchronizations(lanes[i], original_.at(lanes[i]), tails[i], future_);
    }
}

HiCacheExpandedHost HiCacheHostSequence::append(const HiCacheHostExpansion & plan, std::span<const size_t> lanes, std::span<const size_t> event_completions) {
    auto result = expand_host(plan, resolve(lanes), future_, host_return_, event_completions);
    advance(lanes, result.stream_tails, result.host_return);
    return result;
}

HiCacheExpandedWrite HiCacheHostSequence::append(const HiCacheWriteExpansion & plan, uint64_t service_us, std::span<const size_t> lanes) {
    auto result = expand_write(plan, service_us, resolve(lanes), future_, host_return_);
    advance(lanes, result.stream_tails, result.host_return);
    return result;
}

HiCacheWriteStreamPosition HiCacheWriteStreamInsertions::position(size_t lane, HiCacheWriteStreamPosition source) const {
    const auto found = tails_.find(lane);
    if (found != tails_.end() && found->second.source.before == source.before && found->second.source.after == source.after) source.before = found->second.tail;
    return source;
}

void HiCacheWriteStreamInsertions::advance(size_t lane, HiCacheWriteStreamPosition source, size_t tail) {
    tails_.insert_or_assign(lane, Entry{ source, tail });
}

void HiCacheWriteStreamInsertions::bind_synchronizations(const core::DagGraph & graph) {
    graph_ = &graph;
    std::map<size_t, std::set<size_t>> consumers;
    for (const auto & edge : graph.edges()) {
        if (!edge.active || edge.kind != core::DagEdgeKind::Sync || !graph.node(edge.src).active || !graph.node(edge.dst).active || graph.node(edge.src).is_cpu
            || !graph.node(edge.dst).is_cpu || !core::synchronizes_device_frontier(graph.event_for_node(edge.dst)))
            continue;
        consumers[graph.node(edge.src).lane_id].insert(edge.dst);
    }
    synchronizations_.clear();
    for (const auto & [lane, nodes] : consumers) synchronizations_[lane] = { nodes.begin(), nodes.end() };
}

void HiCacheWriteStreamInsertions::constrain_synchronizations(size_t lane, const HiCacheWriteStreamPosition & source, size_t tail,
                                                              simulation::FutureDag & future) const {
    if (!graph_) return;
    const auto found = synchronizations_.find(lane);
    if (found == synchronizations_.end()) return;
    if (!source.submission_site) throw std::runtime_error("New stream work needs its CPU insertion identity for downstream synchronizations");
    const auto & site = *source.submission_site;
    for (const auto node : found->second) {
        if (!graph_->node(node).active) continue;
        const auto & event = graph_->event_for_node(node);
        if (event.pid != site.pid || event.tid != site.tid) throw std::runtime_error("New stream work has a synchronization on another CPU thread");
        if (event.ts < site.timestamp_us) continue;
        if (event.ts == site.timestamp_us) throw std::runtime_error("Stream insertion and synchronization share an unresolved CPU boundary");
        future.depend(tail, node, core::DagEdgeKind::Sync);
    }
}


namespace {
HiCacheExpandedHost expand_host_work(const HiCacheHostExpansion & plan, std::span<const HiCacheHostExpansion::Node> work,
                                     std::span<const HiCacheWriteStreamPosition> positions, simulation::FutureDag & future,
                                     std::optional<size_t> previous_host_return, std::span<const size_t> event_completions) {
    if (event_completions.size() != plan.event_waits.size()) throw std::invalid_argument("Host expansion requires explicit target event completions");
    if (positions.size() != plan.streams.size() + plan.waits.size()) throw std::invalid_argument("Host expansion requires resolved stream and wait positions");

    std::vector<size_t> nodes(work.size(), core::DagNode::kNoNode);
    for (size_t i = 0; i < work.size(); ++i) {
        const auto & node = work[i];
        nodes[i] = node.submission ? future.append_cpu_task(node.work, nodes.at(*node.submission), *node.queue_member) : future.append(node.work);
    }

    for (const auto & edge : plan.edges) future.depend(nodes.at(edge.from), nodes.at(edge.to), edge.kind);
    if (previous_host_return) future.depend(*previous_host_return, nodes.front());
    HiCacheExpandedHost result{ nodes.at(plan.host_return), {}, {} };
    for (size_t i = 0; i < plan.streams.size(); ++i) {
        if (positions[i].before) future.depend(*positions[i].before, nodes.at(plan.streams[i].first));
        if (positions[i].after) future.depend(nodes.at(plan.streams[i].last), *positions[i].after);
        result.stream_tails.push_back(nodes.at(plan.streams[i].last));
    }
    for (size_t i = 0; i < plan.waits.size(); ++i)
        if (positions[plan.streams.size() + i].before) future.depend(*positions[plan.streams.size() + i].before, nodes.at(plan.waits[i].consumer));
    for (size_t i = 0; i < plan.event_waits.size(); ++i) future.depend(event_completions[i], nodes.at(plan.event_waits[i].consumer));
    result.nodes = std::move(nodes);
    return result;
}
} // namespace

HiCacheExpandedHost expand_host(const HiCacheHostExpansion & plan, std::span<const HiCacheWriteStreamPosition> positions, simulation::FutureDag & future,
                                std::optional<size_t> previous_host_return, std::span<const size_t> event_completions) {
    return expand_host_work(plan, plan.nodes, positions, future, previous_host_return, event_completions);
}

HiCacheExpandedWrite expand_write(const HiCacheWriteExpansion & plan, uint64_t service_us, std::span<const HiCacheWriteStreamPosition> positions,
                                  simulation::FutureDag & future, std::optional<size_t> previous_host_return) {
    if (!plan.payload_bytes) throw std::invalid_argument("Write expansion requires payload geometry");
    // Only node costs vary per call; topology and source/resource bindings are
    // borrowed unchanged. Prefix rounding preserves the total device service.
    auto work = plan.nodes;
    uint64_t bytes = 0, assigned = 0;
    for (const auto & [node, count] : plan.payload) {
        bytes += count;
        const auto prefix = core::floor_multiply_divide_u64(service_us, bytes, plan.payload_bytes).value();
        work.at(node).work.duration = prefix - assigned;
        assigned = prefix;
    }

    auto host = expand_host_work(plan, work, positions, future, previous_host_return, {});
    return { host.host_return, host.nodes.at(plan.write_start), host.nodes.at(plan.completion), std::move(host.stream_tails), std::move(host.nodes) };
}

} // namespace markov::trace_graph::modules::hicache::runtime
