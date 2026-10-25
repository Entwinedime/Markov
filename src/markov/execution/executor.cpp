#include "markov/execution/executor.hpp"

#include <algorithm>
#include <deque>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace markov {

Executor::Executor(const NodeDurations & durations) : durations_(durations) {}

void Executor::submit(const RegionGraph & graph, const TaskIdentity & task, const BatchProgram & program) {
    if (program.completions.empty()) throw std::invalid_argument("work needs a completion port");
    if (tasks_.contains(task)) throw std::invalid_argument("control reused an in-flight task identity");
    ingest(graph);
    tasks_.emplace(task, Task{ next_submission_++, program.completions.size(), program.publication.has_value() });

    // Register both ports before executing zero-duration work.
    if (program.publication) watch(*program.publication, { task, ExecutionEvent::Kind::Published });
    for (const auto completion : program.completions) watch(completion, { task, ExecutionEvent::Kind::Completed });
    start_ready();
}

void Executor::ingest(const RegionGraph & graph) {
    const auto & execution = graph.execution;
    if (!execution.incoming_waits().empty()) {
        const auto & wait = execution.incoming_waits().front();
        const auto & owner = execution.nodes()[wait.target - execution.first_node()].owner;
        const auto region = owner.kind == ExecutionOwner::Kind::Region ? owner.identity : graph.operations.at(owner.identity).region;
        const auto & scope = graph.regions.at(region);
        auto root = region;
        while (!graph.regions[root].at("parent").is_null()) root = graph.regions[root].at("parent").get<std::size_t>();
        throw std::invalid_argument("execution has unresolved wait: "
                                    + nlohmann::json{ { "kind", wait.kind }, { "resource", wait.resource }, { "target", wait.target },
                                                      { "role", graph.regions[root].at("role") },
                                                      { "operation", owner.kind == ExecutionOwner::Kind::Operation ? nlohmann::json(graph.operations.at(owner.identity).name) : nlohmann::json(nullptr) },
                                                      { "rank", scope.value("rank", nlohmann::json(nullptr)) },
                                                      { "batch", scope.value("batch", nlohmann::json(nullptr)) } }.dump());
    }
    if (execution.has_unbound_accesses(graph.operations.size())) throw std::invalid_argument("execution has unresolved data access ports");
    if (execution.has_unbound_communications()) throw std::invalid_argument("execution has unresolved collective participants");

    const auto begin = first_node_ + work_.size();
    const auto nodes = execution.nodes().subspan(begin - execution.first_node());
    std::vector<Work> added;
    for (const auto & node : nodes) {
        const auto duration = durations_.duration_ns(graph, node);
        if (duration < 0) throw std::invalid_argument("execution duration is negative");
        added.push_back({ .resource = node.step.resource, .duration_ns = duration, .started_ns = std::nullopt, .successors = {}, .notifications = {} });
    }

    // Validate the appended DAG before committing it; old predecessors are already part of a valid graph.
    const auto edges = execution.dependencies().subspan(consumed_edges_);
    std::vector<std::size_t> remaining(added.size());
    for (const auto & edge : edges) {
        if (edge.target < begin) throw std::invalid_argument("new dependencies cannot change already submitted work");
        auto & target = added.at(edge.target - begin);
        if (edge.source >= begin) {
            added.at(edge.source - begin).successors.push_back(edge.target);
            ++remaining.at(edge.target - begin);
            ++target.remaining;
        }
        else if (!completed(edge.source)) ++target.remaining;
    }

    std::deque<std::size_t> ready_nodes;
    for (std::size_t index = 0; index < remaining.size(); ++index) {
        if (remaining[index] == 0) ready_nodes.push_back(index);
    }
    std::size_t visited = 0;
    while (!ready_nodes.empty()) {
        const auto node = ready_nodes.front();
        ready_nodes.pop_front();
        for (const auto successor : added[node].successors) {
            if (--remaining[successor - begin] == 0) ready_nodes.push_back(successor - begin);
        }
        ++visited;
    }
    if (visited != added.size()) throw std::invalid_argument("execution dependencies contain a cycle");

    work_.insert(work_.end(), std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
    for (const auto & edge : edges) {
        if (edge.source < begin && !completed(edge.source)) work_.at(edge.source - first_node_).successors.push_back(edge.target);
    }
    for (std::size_t node = begin; node < first_node_ + work_.size(); ++node) {
        if (work_[node - first_node_].remaining == 0) ready(node);
    }
    consumed_edges_ = execution.dependencies().size();
}

bool Executor::completed(std::size_t node) const {
    if (node >= first_node_) return work_.at(node - first_node_).completed;
    if (!completed_frontiers_.contains(node)) throw std::out_of_range("execution references a retired node without a retained frontier");
    return true;
}

void Executor::ready(std::size_t node) { resources_[work_.at(node - first_node_).resource].ready.emplace(now_ns_, node); }

void Executor::start_ready() {
    for (auto & [resource, state] : resources_) {
        if (state.running || state.ready.empty()) continue;
        const auto node = state.ready.begin()->second;
        auto & work = work_.at(node - first_node_);
        if (work.duration_ns > std::numeric_limits<std::int64_t>::max() - now_ns_) throw std::invalid_argument("execution duration overflows simulated time");

        state.ready.erase(state.ready.begin());
        state.running = node;
        work.started_ns = now_ns_;
        completions_.emplace(now_ns_ + work.duration_ns, node);
    }
}

void Executor::watch(std::size_t node, Notification notification) {
    if (completed(node)) notify(notification);
    else work_.at(node - first_node_).notifications.push_back(std::move(notification));
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
            auto & work = work_.at(node - first_node_);
            work.completed = true;
            resources_.at(work.resource).running.reset();

            for (const auto successor : work.successors) {
                if (--work_.at(successor - first_node_).remaining == 0) ready(successor);
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
            if (!tasks_.empty()) throw std::invalid_argument("execution has blocked work without a completion");
            if (deadline_ns) now_ns_ = *deadline_ns;
            return std::nullopt;
        }
        if (deadline_ns && *deadline_ns < completions_.begin()->first) {
            now_ns_ = *deadline_ns;
            return std::nullopt;
        }
        complete_work();
    }
}

std::int64_t Executor::idle_until(const RegionGraph & graph, std::span<const std::size_t> completions, std::int64_t time_ns) {
    if (!tasks_.empty()) throw std::logic_error("idle advancement cannot skip task feedback");
    if (time_ns < now_ns_) throw std::invalid_argument("execution time must not move backwards");
    ingest(graph);
    start_ready();
    while (!std::ranges::all_of(completions, [this](auto node) { return completed(node); })) complete_work();
    const auto ready_ns = now_ns_;
    const auto until_ns = std::max(time_ns, ready_ns);
    while (!completions_.empty() && completions_.begin()->first <= until_ns) complete_work();
    now_ns_ = until_ns;
    return ready_ns;
}

void Executor::retain(std::span<const std::size_t> frontiers) {

    // Keep unfinished tails with their durations and successors after the source graph releases its history.
    while (!work_.empty() && work_.front().completed) {
        work_.pop_front();
        ++first_node_;
    }
    completed_frontiers_.clear();
    for (const auto node : frontiers) {
        if (node < first_node_) completed_frontiers_.insert(node);
    }
    consumed_edges_ = 0;
}

std::int64_t Executor::now_ns() const { return now_ns_; }

nlohmann::json Executor::timeline() const {
    auto result = nlohmann::json::array();
    for (std::size_t index = 0; index < work_.size(); ++index) {
        const auto & work = work_[index];
        nlohmann::json record{
            {   "node",first_node_ + index    },
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
