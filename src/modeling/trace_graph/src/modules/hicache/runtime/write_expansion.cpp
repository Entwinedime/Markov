#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/cost/host_operation.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace markov::trace_graph::modules::hicache::runtime {
namespace {

void bind_cpu_service(core::DagSyntheticNodeSpec & work, double cpu_us, double residual_us, std::pair<double, double> & remainders) {
    const auto cpu = core::truncate_to_u64(cpu_us + remainders.first);
    const auto residual = core::truncate_to_u64(residual_us + remainders.second);
    if (cpu_us < 0 || residual_us < 0 || !cpu || !residual) throw std::invalid_argument("Generated CPU control requires finite nonnegative cost estimates");

    remainders.first += cpu_us - *cpu;
    remainders.second += residual_us - *residual;
    work.duration = *cpu;
    work.cpu_gap_after = *residual;
}

} // namespace

size_t HiCacheHostExpansion::append_main(std::string_view lane, std::string name, std::optional<size_t> & previous, std::string category) {
    const auto node = nodes.size();
    nodes.push_back({
        { .name = std::move(name), .category = std::move(category), .lane_key = std::string(lane) }
    });
    if (previous) edges.push_back({ *previous, node, core::DagEdgeKind::Sequential });

    previous = node;
    return node;
}

HiCacheHostExpansion generate_cpu_control(std::string_view lane, std::string_view name) {
    HiCacheHostExpansion plan;
    std::optional<size_t> previous;
    (void)plan.append_main(lane, std::string(name), previous);
    plan.host_return = plan.append_main(lane, "target control return", previous, "");
    return plan;
}

void bind_cpu_control(HiCacheHostExpansion & plan, double cpu_us, double residual_us, std::pair<double, double> & remainders) {
    bind_cpu_service(plan.nodes.front().work, cpu_us, residual_us, remainders);
    plan.nodes.front().work.attrs["cost_binding_source"] = "hicache_control_independent_model";
}

std::map<size_t, size_t> observe_host_worker_members(const core::DagGraph & graph) {
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    std::map<size_t, size_t> members;
    for (const auto & task : queues.tasks) members.emplace(graph.node(task.first).lane_id, task.first);
    return members;
}

void rebind_host_stream_position(const adapters::sglang::HiCacheSourceIndex & source, HiCacheWriteStreamPosition & position, const std::string & pid,
                                 const std::string & tid, uint64_t at_us) {
    const auto & graph = source.graph();
    if ((!position.before || graph.node(*position.before).active) && (!position.after || graph.node(*position.after).active)) return;

    const auto sample = position.before ? *position.before : *position.after;
    position = observe_write_stream_position(source, sample, pid, tid, at_us);
}

void rebind_host_stream_positions(const adapters::sglang::HiCacheSourceIndex & source, std::map<size_t, HiCacheWriteStreamPosition> & positions,
                                  const std::string & pid, const std::string & tid, uint64_t at_us) {
    for (auto & [lane, position] : positions) rebind_host_stream_position(source, position, pid, tid, at_us);
}

void rebind_host_worker_queues(const core::DagGraph & graph, const std::map<size_t, size_t> & members, std::span<HiCacheHostExpansion *> plans) {
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

HiCacheExpandedWrite HiCacheHostSequence::append(const HiCacheWriteExpansion & plan, std::span<const size_t> lanes) {
    auto result = expand_write(plan, resolve(lanes), future_, host_return_);
    advance(lanes, result.stream_tails, result.host_return);
    return result;
}

HiCacheWriteStreamPosition HiCacheWriteStreamInsertions::position(size_t lane, HiCacheWriteStreamPosition source) const {
    const auto found = tails_.find({ lane, source.before, source.after });
    if (found != tails_.end()) source.before = found->second;
    return source;
}

void HiCacheWriteStreamInsertions::advance(size_t lane, HiCacheWriteStreamPosition source, size_t tail) {
    tails_.insert_or_assign(Cut{ lane, source.before, source.after }, tail);
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
HiCacheExpandedHost expand_host_work(const HiCacheHostExpansion & plan, std::span<const HiCacheWriteStreamPosition> positions, simulation::FutureDag & future,
                                     std::optional<size_t> previous_host_return, std::span<const size_t> event_completions,
                                     std::span<const size_t> extra_observers = {}) {
    if (event_completions.size() != plan.event_waits.size()) throw std::invalid_argument("Host expansion requires explicit target event completions");
    if (positions.size() != plan.streams.size() + plan.waits.size()) throw std::invalid_argument("Host expansion requires resolved stream and wait positions");

    std::vector<bool> observers(plan.nodes.size(), false);
    observers.at(plan.host_return) = true;
    for (const auto & stream : plan.streams) {
        observers.at(stream.first) = true;
        observers.at(stream.last) = true;
    }
    for (const auto * waits : { &plan.waits, &plan.event_waits, &plan.collective_completions })
        for (const auto & wait : *waits) observers.at(wait.consumer) = true;
    for (const auto local : extra_observers) observers.at(local) = true;

    std::vector<size_t> nodes(plan.nodes.size(), core::DagNode::kNoNode);
    for (size_t i = 0; i < plan.nodes.size(); ++i) {
        const auto & node = plan.nodes[i];
        const bool observe = observers[i] || node.observe_start;
        if (node.task_predecessor) nodes[i] = future.append_cpu_task_part(node.work, nodes.at(*node.task_predecessor), observe);
        else if (node.submission) nodes[i] = future.append_cpu_task(node.work, nodes.at(*node.submission), *node.queue_member, observe);
        else nodes[i] = future.append(node.work, simulation::BoundaryOrder::Ordinary, std::nullopt, observe);
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
    for (const auto & wait : plan.collective_completions) future.depend(wait.source_node, nodes.at(wait.consumer), core::DagEdgeKind::HCCL);
    result.nodes = std::move(nodes);
    return result;
}
} // namespace

HiCacheExpandedHost expand_host(const HiCacheHostExpansion & plan, std::span<const HiCacheWriteStreamPosition> positions, simulation::FutureDag & future,
                                std::optional<size_t> previous_host_return, std::span<const size_t> event_completions) {
    return expand_host_work(plan, positions, future, previous_host_return, event_completions);
}

void bind_transfer_payload(HiCacheWriteExpansion & plan, uint64_t service_us) {
    if (!plan.payload_bytes) throw std::invalid_argument("Write expansion requires payload geometry");

    uint64_t bytes = 0, assigned = 0;
    for (const auto & [node, count] : plan.payload) {
        bytes += count;
        const auto prefix = core::floor_multiply_divide_u64(service_us, bytes, plan.payload_bytes).value();

        auto & work = plan.nodes.at(node).work;
        work.duration = prefix - assigned;
        work.attrs["cost_binding_source"] = "hicache_payload_model";
        assigned = prefix;
    }
}

HiCacheExpandedWrite expand_write(const HiCacheWriteExpansion & plan, std::span<const HiCacheWriteStreamPosition> positions, simulation::FutureDag & future,
                                  std::optional<size_t> previous_host_return) {
    const size_t observers[]{ plan.write_start, plan.completion };
    auto host = expand_host_work(plan, positions, future, previous_host_return, {}, observers);
    return { host.host_return, host.nodes.at(plan.write_start), host.nodes.at(plan.completion), std::move(host.stream_tails), std::move(host.nodes) };
}

} // namespace markov::trace_graph::modules::hicache::runtime
