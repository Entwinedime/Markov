#include "markov/execution/executor.hpp"

#include <algorithm>
#include <deque>
#include <limits>
#include <stdexcept>
#include <utility>

namespace markov {

Executor::Executor(const NodeDurations & durations) : durations_(durations) {}

void Executor::submit(const RegionGraph & graph, const ServingAction & action, const BatchProgram & program) {
    const auto & task = action.task;
    ingest(graph);
    tasks_.emplace(task, Task{ next_submission_++, program.completions.size(), program.publication.has_value() });

    // Register both ports before executing zero-duration work.
    if (program.publication) watch(*program.publication, { task, ExecutionEvent::Kind::Published });
    for (const auto completion : program.completions) watch(completion, { task, ExecutionEvent::Kind::Completed });
    start_ready();
}

void Executor::ingest(const RegionGraph & graph) {
    const auto & execution = graph.execution;

    std::set<std::size_t> awaiting_peer;
    for (const auto & [_, ports] : execution.pending_communications()) awaiting_peer.insert(ports.returned);

    const auto begin = next_node_;
    const auto nodes = execution.nodes().subspan(begin - execution.first_node());
    std::vector<Work> added;
    for (const auto & node : nodes) {
        const auto duration = durations_.duration_ns(graph, node);
        if (duration < 0) throw std::invalid_argument("execution duration is negative");
        added.push_back({ .resource = node.step.resource,
                          .duration_ns = duration,
                          .started_ns = std::nullopt,
                          .awaiting_peer = awaiting_peer.contains(begin + added.size()),
                          .successors = {},
                          .notifications = {} });
    }

    // Only a matched communication may add a predecessor to an already submitted, explicitly blocked port.
    const auto edges = execution.dependencies().subspan(consumed_edges_);
    std::set<std::size_t> released;
    for (const auto & edge : edges) {
        if (edge.source >= begin + added.size() || edge.target >= begin + added.size())
            throw std::invalid_argument("execution dependency references an undefined node");
        if (edge.target >= begin) continue;
        const auto target = work_.find(edge.target);
        if (target == work_.end() || !target->second.awaiting_peer || awaiting_peer.contains(edge.target) || edge.source < begin
            || execution.nodes()[edge.source - execution.first_node()].step.kind != ExecutionKind::Communication || !released.insert(edge.target).second)
            throw std::invalid_argument("new dependencies can only resolve a pending communication port");
    }

    // Late peers can close a cycle through older unfinished work; validate that whole live graph only in this case.
    std::map<std::size_t, std::vector<std::size_t>> successors;
    if (!released.empty()) {
        for (const auto & [node, work] : work_) {
            if (!work.completed) successors.emplace(node, work.successors);
        }
    }
    for (std::size_t node = begin; node < begin + added.size(); ++node) successors.try_emplace(node);
    for (const auto & edge : edges) {
        if (const auto source = successors.find(edge.source); source != successors.end()) source->second.push_back(edge.target);
        else completed(edge.source);
    }

    std::map<std::size_t, std::size_t> remaining;
    for (const auto & [node, _] : successors) remaining.emplace(node, 0);
    for (const auto & [_, targets] : successors) {
        for (const auto target : targets) ++remaining.at(target);
    }
    std::deque<std::size_t> ready_nodes;
    for (const auto & [node, count] : remaining) {
        if (count == 0) ready_nodes.push_back(node);
    }
    std::size_t visited = 0;
    while (!ready_nodes.empty()) {
        const auto node = ready_nodes.front();
        ready_nodes.pop_front();
        for (const auto successor : successors.at(node)) {
            if (--remaining.at(successor) == 0) ready_nodes.push_back(successor);
        }
        ++visited;
    }
    if (visited != successors.size()) throw std::invalid_argument("execution dependencies contain a cycle");

    for (auto & work : added) work_.emplace(next_node_++, std::move(work));
    for (const auto & edge : edges) {
        if (!completed(edge.source)) {
            work_.at(edge.source).successors.push_back(edge.target);
            ++work_.at(edge.target).remaining;
        }
    }
    for (const auto node : released) {
        auto & work = work_.at(node);
        work.awaiting_peer = false;
        if (work.remaining == 0) ready(node);
    }
    for (std::size_t node = begin; node < next_node_; ++node) {
        if (work_.at(node).remaining == 0) ready(node);
    }
    consumed_edges_ = execution.dependencies().size();
}

bool Executor::completed(std::size_t node) const {
    if (const auto work = work_.find(node); work != work_.end()) return work->second.completed;
    if (!completed_frontiers_.contains(node)) throw std::out_of_range("execution references a retired node without a retained frontier");
    return true;
}

void Executor::ready(std::size_t node) {
    const auto & work = work_.at(node);
    if (!work.awaiting_peer) resources_[work.resource].ready.emplace(now_ns_, node);
}

void Executor::start_ready() {
    for (auto & [resource, state] : resources_) {
        if (state.running || state.ready.empty()) continue;
        const auto node = state.ready.begin()->second;
        auto & work = work_.at(node);
        if (work.duration_ns > std::numeric_limits<std::int64_t>::max() - now_ns_) throw std::invalid_argument("execution duration overflows simulated time");

        state.ready.erase(state.ready.begin());
        state.running = node;
        work.started_ns = now_ns_;
        completions_.emplace(now_ns_ + work.duration_ns, node);
    }
}

void Executor::watch(std::size_t node, Notification notification) {
    if (completed(node)) notify(notification);
    else work_.at(node).notifications.push_back(std::move(notification));
}

void Executor::notify(const Notification & notification) {
    auto & task = tasks_.at(notification.task);
    if (notification.kind == ExecutionEvent::Kind::Published) task.published = true;
    else if (--task.remaining != 0) return;

    feedback_.emplace(std::tuple{ now_ns_, task.order, notification.kind }, ExecutionEvent{ now_ns_, notification.task, notification.kind });
}

void Executor::complete_work() {
    if (completions_.empty()) throw std::invalid_argument("execution has blocked work without a completion");
    now_ns_ = completions_.begin()->first;
    do {
        while (!completions_.empty() && completions_.begin()->first == now_ns_) {
            const auto node = completions_.begin()->second;
            completions_.erase(completions_.begin());
            auto & work = work_.at(node);
            work.completed = true;
            completed_work_.push_back(node);

            // Ordering frontiers belong to the graph; an idle resource has no executor state to retain.
            auto & resource = resources_.at(work.resource);
            resource.running.reset();
            if (resource.ready.empty()) resources_.erase(work.resource);

            for (const auto successor : work.successors) {
                if (--work_.at(successor).remaining == 0) ready(successor);
            }
            for (const auto & notification : work.notifications) notify(notification);
            work.successors.clear();
            work.notifications.clear();
        }

        // Settle zero-duration work before delivering feedback at this same simulated instant.
        start_ready();
    } while (!completions_.empty() && completions_.begin()->first == now_ns_);
}

std::optional<ExecutionEvent> Executor::advance_until(std::optional<std::int64_t> deadline_ns) {
    if (deadline_ns && *deadline_ns < now_ns_) throw std::invalid_argument("execution time must not move backwards");
    while (true) {
        if (!feedback_.empty()) {
            const auto event = feedback_.begin()->second;
            if (event.kind == ExecutionEvent::Kind::Completed) {
                const auto & task = tasks_.at(event.task);
                if (task.expects_publication && !task.published) throw std::invalid_argument("task completion precedes its publication");
                tasks_.erase(event.task);
            }
            feedback_.erase(feedback_.begin());
            return event;
        }
        if (completions_.empty()) {
            if (deadline_ns) now_ns_ = *deadline_ns;
            else if (!tasks_.empty()) throw std::invalid_argument("execution has blocked work without a completion");
            return std::nullopt;
        }
        if (deadline_ns && *deadline_ns < completions_.begin()->first) {
            now_ns_ = *deadline_ns;
            return std::nullopt;
        }
        complete_work();
    }
}

void Executor::retain(std::span<const std::size_t> frontiers) {

    std::set<std::size_t> completed_frontiers;
    for (const auto node : frontiers) {
        if (completed(node)) completed_frontiers.insert(node);
    }

    // Each completion is retired once; long-running work need not be scanned on every control return.
    for (const auto node : completed_work_) work_.erase(node);
    completed_work_.clear();
    completed_frontiers_ = std::move(completed_frontiers);
    consumed_edges_ = 0;
}

std::int64_t Executor::now_ns() const { return now_ns_; }

nlohmann::json Executor::finish(bool details) const {
    nlohmann::json result{
        { "completion_source",         "execution_ports" },
        {   "duration_source", "given_execution_kind_ns" }
    };
    if (details) result["execution_timeline"] = timeline();
    return result;
}

nlohmann::json Executor::timeline() const {
    auto result = nlohmann::json::array();
    for (const auto & [node, work] : work_) {
        nlohmann::json record{
            {   "node",                  node },
            { "status",
             work.completed    ? "completed"
             : work.started_ns ? "running"
             : "waiting" }
        };
        if (work.started_ns) {
            record["started_ns"] = *work.started_ns;
            record[work.completed ? "completed_ns" : "expected_ns"] = *work.started_ns + work.duration_ns;
        }
        result.push_back(std::move(record));
    }
    return result;
}

} // namespace markov
