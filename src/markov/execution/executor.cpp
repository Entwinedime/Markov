#include "markov/execution/executor.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace markov {

Executor::Executor(const NodeDurations & durations) : durations_(durations) {}

void Executor::submit(const RegionGraph & graph, const BatchProgram & program) {
    const auto completed = evaluate(graph, program.completions);

    // Publication and result completion are distinct graph ports, even when their times coincide.
    const auto published = finish_time(program.publication);
    if (published > completed) throw std::invalid_argument("batch result completion precedes its publication");
    events_.push_back({ ExecutionEvent::Kind::Published, published });
    events_.push_back({ ExecutionEvent::Kind::Completed, completed });
}

std::int64_t Executor::evaluate(const RegionGraph & graph, std::span<const std::size_t> completions) {
    const auto & execution = graph.execution;
    if (execution.has_unbound_waits()) throw std::invalid_argument("execution has unresolved incoming waits");
    if (execution.has_unbound_accesses(graph.operations.size())) throw std::invalid_argument("execution has unresolved data access ports");
    if (execution.has_unbound_communications()) throw std::invalid_argument("execution has unresolved collective participants");

    const auto nodes = execution.nodes();
    const auto begin = first_node_ + finished_ns_.size();
    const auto node_offset = begin - execution.first_node();
    const auto count = nodes.size() - node_offset;
    std::vector<std::size_t> remaining(count);
    std::vector<std::vector<std::size_t>> successors(count);
    std::vector<std::int64_t> ready_ns(count, now_ns_);

    // Appending a graph only adds edges into new work; old predecessors already have finish times.
    const auto edges = execution.dependencies();
    for (const auto & edge : edges.subspan(consumed_edges_)) {
        const auto target = edge.target - begin;
        if (edge.source < begin) ready_ns.at(target) = std::max(ready_ns.at(target), finish_time(edge.source));
        else {
            successors.at(edge.source - begin).push_back(target);
            ++remaining.at(target);
        }
    }

    std::deque<std::size_t> ready;
    for (std::size_t local = 0; local < count; ++local) {
        if (remaining[local] == 0) ready.push_back(local);
    }

    finished_ns_.resize(finished_ns_.size() + count);
    std::size_t processed = 0;
    while (!ready.empty()) {
        const auto local = ready.front();
        ready.pop_front();
        const auto duration = durations_.duration_ns(graph, nodes[node_offset + local]);
        if (duration < 0 || duration > std::numeric_limits<std::int64_t>::max() - ready_ns[local])
            throw std::invalid_argument("execution duration is negative or overflows simulated time");

        const auto finish = ready_ns[local] + duration;
        finished_ns_[begin + local - first_node_] = finish;
        for (const auto next : successors[local]) {
            ready_ns[next] = std::max(ready_ns[next], finish);
            if (--remaining[next] == 0) ready.push_back(next);
        }
        ++processed;
    }
    if (processed != count) throw std::invalid_argument("execution dependencies contain a cycle");

    consumed_edges_ = edges.size();
    auto completed = now_ns_;
    for (const auto completion : completions) completed = std::max(completed, finish_time(completion));
    return completed;
}

std::int64_t Executor::finish_time(std::size_t node) const { return node < first_node_ ? frontier_ns_.at(node) : finished_ns_.at(node - first_node_); }

void Executor::retain(std::span<const std::size_t> frontiers) {
    std::map<std::size_t, std::int64_t> retained;
    for (const auto node : frontiers) retained.emplace(node, finish_time(node));
    frontier_ns_ = std::move(retained);
    first_node_ += finished_ns_.size();
    finished_ns_.clear();
    consumed_edges_ = 0;
}

bool Executor::has_events() const { return !events_.empty(); }

ExecutionEvent Executor::next_event() {
    const auto event = events_.front();
    events_.pop_front();
    now_ns_ = event.time_ns;
    return event;
}

void Executor::advance_to(std::int64_t time_ns) { now_ns_ = time_ns; }
std::int64_t Executor::now_ns() const { return now_ns_; }

} // namespace markov
