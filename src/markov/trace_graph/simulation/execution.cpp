#include "markov/trace_graph/simulation/execution.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace markov::trace_graph::simulation {

bool Execution::Scheduled::operator>(const Scheduled & other) const { return std::tie(at, kind, identity) > std::tie(other.at, other.kind, other.identity); }

Execution::Execution(const graph::Graph & graph, std::span<const graph::Time> initial_availability_ns)
    : source_(&graph),
      original_nodes_(graph.nodes().size()),
      result_{
          { initial_availability_ns.begin(), initial_availability_ns.end() },
          std::vector<graph::Time>(original_nodes_),
          std::vector<OperationState>(original_nodes_, OperationState::Pending),
          0
},
      incoming_(graph.incoming_counts().begin(), graph.incoming_counts().end()) {
    std::size_t resources = 0;
    for (const auto & operation : graph.nodes()) resources = std::max(resources, operation.resource + 1);
    lanes_.resize(resources);
    for (graph::NodeId id = 0; id < original_nodes_; ++id) {
        if (incoming_[id] == 0) ready(id);
    }
}

Execution::Execution(std::size_t resources, EventReceiver & receiver) : receiver_(&receiver), lanes_(resources) {}

const graph::Node & Execution::node(graph::NodeId id) const {
    return id < original_nodes_ ? source_->nodes()[id] : owned_[id - original_nodes_].operation.node;
}

graph::NodeId Execution::submit(Mutation mutation) {
    const auto begin = next_node();
    const auto end = begin + mutation.operations.size();
    std::vector<std::size_t> incoming(mutation.operations.size(), 0), local_incoming(mutation.operations.size(), 0);
    std::vector<std::vector<graph::NodeId>> successors(mutation.operations.size());
    graph::canonicalize_edges(mutation.dependencies);
    for (const auto & operation : mutation.operations) {
        if (operation.node.duration_ns < 0 || operation.node.resource >= lanes_.size())
            throw std::runtime_error("Invalid generated operation cost or resource");
        if (operation.node.kind == graph::NodeKind::Boundary && operation.node.duration_ns != 0)
            throw std::runtime_error("A semantic boundary cannot contain service time");
    }
    for (const auto & edge : mutation.dependencies) {
        if (edge.to < begin || edge.to >= end || edge.from >= end) throw std::runtime_error("Mutation dependencies must target the new operation batch");
        if (edge.from < begin && result_.states[edge.from] == OperationState::Cancelled)
            throw std::runtime_error("Generated work depends on a cancelled operation");
        if (edge.from >= begin || result_.states[edge.from] != OperationState::Complete) ++incoming[edge.to - begin];
        if (edge.from >= begin) {
            ++local_incoming[edge.to - begin];
            successors[edge.from - begin].push_back(edge.to - begin);
        }
    }

    std::vector<graph::NodeId> ready_nodes;
    for (graph::NodeId id = 0; id < local_incoming.size(); ++id) {
        if (local_incoming[id] == 0) ready_nodes.push_back(id);
    }
    for (std::size_t at = 0; at < ready_nodes.size(); ++at) {
        for (const auto next : successors[ready_nodes[at]]) {
            if (--local_incoming[next] == 0) ready_nodes.push_back(next);
        }
    }
    if (ready_nodes.size() != mutation.operations.size()) throw std::runtime_error("Generated operation batch contains a cycle");

    // No graph or execution state changes until the complete batch is valid.
    result_.starts_ns.resize(end, now_ns_);
    result_.completions_ns.resize(end, 0);
    incoming_.insert(incoming_.end(), incoming.begin(), incoming.end());
    result_.states.resize(end, OperationState::Pending);
    for (auto & operation : mutation.operations) owned_.push_back({ std::move(operation), mutation.owner, {} });
    for (const auto & edge : mutation.dependencies) record_edge(edge);
    for (auto id = begin; id < end; ++id) {
        if (incoming_[id] == 0) ready(id);
    }
    return begin;
}

void Execution::record_edge(graph::Edge edge) {
    auto & edges = edge.from < original_nodes_ ? source_additions_[edge.from] : owned_[edge.from - original_nodes_].successors;
    const auto same = [&](const graph::Edge & value) { return value.to == edge.to && value.kind == edge.kind; };
    if (std::ranges::none_of(edges, same)) edges.push_back(edge);
}

std::size_t Execution::schedule_at(graph::Time time_ns, Component owner, Action action) {
    if (time_ns < now_ns_) throw std::runtime_error("Cannot schedule an action before the execution clock");
    const auto id = timers_.size();
    timers_.push_back({ owner, action, true });
    scheduled_.push({ time_ns, ScheduledKind::Timer, id });
    return id;
}

void Execution::cancel_timer(std::size_t timer) { timers_.at(timer).active = false; }

bool Execution::cancel(Component owner, graph::NodeId id) {
    if (id < original_nodes_) throw std::runtime_error("Source operations are immutable");
    const auto & operation = owned_.at(id - original_nodes_);
    if (operation.owner != owner) throw std::runtime_error("A component cannot cancel another component's operation");
    if (result_.states[id] == OperationState::Running || result_.states[id] == OperationState::Complete || result_.states[id] == OperationState::Cancelled)
        return false;
    if (!operation.successors.empty()) throw std::runtime_error("Cancel conditional work before attaching dependent work");

    result_.states[id] = OperationState::Cancelled;
    return true;
}

void Execution::ready(graph::NodeId id) {
    result_.states[id] = OperationState::Ready;
    scheduled_.push({ std::max(now_ns_, result_.starts_ns[id]), ScheduledKind::Available, id });
}

void Execution::available(graph::NodeId id) {
    if (result_.states[id] == OperationState::Cancelled) return;
    if (node(id).kind == graph::NodeKind::Boundary || node(id).kind == graph::NodeKind::Delay) {
        start(id);
        return;
    }

    const auto resource = node(id).resource;
    result_.states[id] = OperationState::Queued;
    lanes_[resource].ready.push_back(id);
    start_next(resource);
}

void Execution::start_next(std::size_t resource) {
    auto & lane = lanes_[resource];
    while (!lane.running && !lane.ready.empty()) {
        const auto id = lane.ready.front();
        lane.ready.pop_front();
        if (result_.states[id] == OperationState::Cancelled) continue;

        if (lane.previous && id >= original_nodes_) record_edge({ *lane.previous, id, graph::EdgeKind::ResourceOrder });
        lane.running = id;
        start(id);
    }
}

void Execution::start(graph::NodeId id) {
    const auto duration = node(id).duration_ns;
    if (duration > std::numeric_limits<graph::Time>::max() - now_ns_) throw std::runtime_error("Operation completion exceeds the execution clock range");

    result_.states[id] = OperationState::Running;
    result_.starts_ns[id] = now_ns_;
    result_.completions_ns[id] = now_ns_ + duration;
    if (duration == 0 && node(id).kind == graph::NodeKind::Boundary) complete(id);
    else scheduled_.push({ now_ns_ + duration, ScheduledKind::Completion, id });
}

void Execution::release(const graph::Edge & edge) {
    if (result_.states[edge.to] == OperationState::Cancelled) return;
    result_.starts_ns[edge.to] = std::max(result_.starts_ns[edge.to], now_ns_);
    if (--incoming_[edge.to] == 0) ready(edge.to);
}

void Execution::complete(graph::NodeId id) {
    result_.states[id] = OperationState::Complete;
    ++result_.executed;
    const auto resource = node(id).resource;
    const bool service = node(id).kind != graph::NodeKind::Boundary && node(id).kind != graph::NodeKind::Delay;
    if (service) {
        lanes_[resource].running.reset();
        lanes_[resource].previous = id;
    }
    if (id < original_nodes_) {
        for (const auto & edge : source_->successors(id)) release(edge);
        const auto added = source_additions_.find(id);
        if (added != source_additions_.end()) {
            for (const auto & edge : added->second) release(edge);
        }
    }
    else {
        const auto & operation = owned_[id - original_nodes_];
        for (const auto & edge : operation.successors) release(edge);
        const auto action = operation.operation.completion;
        const auto owner = operation.owner;
        if (action) receiver_->on_event(*this, { owner, *action, EventOrigin::Completion, id });
    }
    if (service && !stopped_) start_next(resource);
}

void Execution::run() {
    while (!scheduled_.empty() && !stopped_) {
        const auto event = scheduled_.top();
        scheduled_.pop();
        if (event.kind == ScheduledKind::Timer && !timers_[event.identity].active) continue;
        if (event.kind == ScheduledKind::Available && result_.states[event.identity] == OperationState::Cancelled) continue;
        now_ns_ = event.at;
        if (event.kind == ScheduledKind::Available) available(event.identity);
        else if (event.kind == ScheduledKind::Completion) complete(event.identity);
        else {
            auto & timer = timers_[event.identity];
            timer.active = false;
            const auto owner = timer.owner;
            const auto action = timer.action;
            receiver_->on_event(*this, { owner, action, EventOrigin::Timer, event.identity });
        }
    }
    if (stopped_) return;

    for (const auto phase : result_.states) {
        if (phase != OperationState::Complete && phase != OperationState::Cancelled)
            throw std::runtime_error("Execution has unresolved dependencies or a cycle");
    }
}

ExecutionResult Execution::take_result() { return std::move(result_); }

} // namespace markov::trace_graph::simulation
