/**
 * @file
 * @brief Compact-CSR topological simulator implementation.
 */
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include "markov/trace_graph/simulation/cpu_task_queues.hpp"

#include "../core/dag_topology.hpp"
#include "markov/trace_graph/core/logger.hpp"
#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <queue>
#include <ranges>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace markov::trace_graph::simulation {

namespace topological_simulator_detail {

constexpr size_t kInvalidNode = std::numeric_limits<size_t>::max();

struct CsrAdjacency {
    std::vector<size_t> offsets;
    std::vector<size_t> destinations;
    std::vector<core::DagEdgeKind> edge_kinds;
};

struct ActiveDagStorage {
    CsrAdjacency outgoing;
    std::vector<size_t> indegree;
    size_t active_edge_count = 0;
};

struct ReplayInterval {
    uint64_t start_us = 0;
    uint64_t end_us = 0;
};

class ControlExclusionIndex {
public:
    ControlExclusionIndex() = default;

    explicit ControlExclusionIndex(const core::DagGraph & graph) {
        for (const auto & interval : graph.control_exclusion_intervals()) {
            if (interval.end_us > interval.start_us) by_logical_input_[interval.gpu_id].push_back({ interval.start_us, interval.end_us });
        }
        for (auto & intervals : by_logical_input_ | std::views::values) merge(intervals);
    }

    [[nodiscard]] uint64_t overlap_us(int logical_input, uint64_t start_us, uint64_t end_us) const {
        if (end_us <= start_us) return 0;
        const auto found = by_logical_input_.find(logical_input);
        if (found == by_logical_input_.end()) return 0;
        const auto & intervals = found->second;
        auto current = std::ranges::lower_bound(intervals, start_us, {}, &ReplayInterval::end_us);
        uint64_t overlap = 0;
        for (; current != intervals.end() && current->start_us < end_us; ++current) {
            const auto begin = std::max(start_us, current->start_us);
            const auto end = std::min(end_us, current->end_us);
            if (end > begin) overlap = core::checked_add_u64(overlap, end - begin, "control exclusion overlap exceeds uint64 range");
        }
        return overlap;
    }

private:
    static void merge(std::vector<ReplayInterval> & intervals) {
        std::ranges::sort(intervals, [](const auto & left, const auto & right) {
            if (left.start_us != right.start_us) return left.start_us < right.start_us;
            return left.end_us < right.end_us;
        });
        size_t output = 0;
        for (const auto & interval : intervals) {
            if (output > 0 && interval.start_us <= intervals[output - 1].end_us) {
                intervals[output - 1].end_us = std::max(intervals[output - 1].end_us, interval.end_us);
                continue;
            }
            intervals[output++] = interval;
        }
        intervals.resize(output);
    }

    std::unordered_map<int, std::vector<ReplayInterval>> by_logical_input_;
};

enum class ReplayMode : std::uint8_t { Full, ControlOnly, GapExcluded };

ActiveDagStorage build_active_dag_storage(const core::DagGraph & graph, const detail::CpuTaskQueues& queues) {
    ActiveDagStorage storage;
    const auto node_count = graph.node_count();
    const auto & nodes = graph.nodes();
    storage.indegree.resize(node_count, 0);
    storage.outgoing.offsets.resize(node_count + 1, 0);
    for (const auto & edge : graph.edges()) {
        if (!edge.active) continue;
        if (edge.src >= node_count || edge.dst >= node_count) throw std::runtime_error("Invalid active DAG: active edge endpoint is out of range");
        if (!nodes[edge.src].active || !nodes[edge.dst].active) throw std::runtime_error("Invalid active DAG: active edge references a disabled node");
        if (queues.replaces(edge)) continue;
        storage.outgoing.offsets[edge.src + 1]++;
        storage.indegree[edge.dst]++;
        storage.active_edge_count++;
    }

    for (size_t node_id = 0; node_id < node_count; ++node_id) storage.outgoing.offsets[node_id + 1] += storage.outgoing.offsets[node_id];
    storage.outgoing.destinations.resize(storage.active_edge_count);
    storage.outgoing.edge_kinds.resize(storage.active_edge_count, core::DagEdgeKind::Sequential);
    auto cursor = storage.outgoing.offsets;
    for (const auto & edge : graph.edges()) {
        if (!edge.active || queues.replaces(edge)) continue;
        const auto offset = cursor[edge.src]++;
        storage.outgoing.destinations[offset] = edge.dst;
        storage.outgoing.edge_kinds[offset] = edge.kind;
    }
    return storage;
}

class TopologicalSimulation : private FutureDag {
public:
    explicit TopologicalSimulation(core::DagGraph & graph, ReplayMode mode, NodeCostAtStart cost_at_start = {}, ExpandAtStart expand_at_start = {})
        : graph_(graph),
          nodes_(graph.nodes()),
          active_node_count_(graph.active_node_count()),
          queues_(mode == ReplayMode::Full ? detail::discover_cpu_task_queues(graph) : detail::CpuTaskQueues{}),
          storage_(build_active_dag_storage(graph, queues_)),
          start_time_(graph.node_count(), 0),
          completion_time_(graph.node_count(), 0),
          workers_(queues_.queue_count),
          task_ready_(queues_.tasks.size(), false),
          task_order_(queues_.queue_count),
          mode_(mode),
          control_exclusions_(mode == ReplayMode::ControlOnly ? ControlExclusionIndex(graph) : ControlExclusionIndex{}),
          cost_at_start_(std::move(cost_at_start)),
          expand_at_start_(std::move(expand_at_start)),
          execution_(graph.node_count(), Execution::Pending),
          ready_generation_(graph.node_count(), 0),
          chronological_(!queues_.tasks.empty() || static_cast<bool>(cost_at_start_) || static_cast<bool>(expand_at_start_)) {
        ready_.reserve(active_node_count_);
        for (const auto & node : nodes_) {
            if (node.active && storage_.indegree[node.id] == 0) make_ready(node.id);
        }
#ifdef DEBUG
        if (mode_ == ReplayMode::GapExcluded) {
            predecessor_.assign(graph.node_count(), kInvalidNode);
            predecessor_edge_kind_.assign(graph.node_count(), core::DagEdgeKind::Sequential);
            predecessor_delay_us_.assign(graph.node_count(), 0);
        }
#endif
    }

    [[nodiscard]] SimulationResult run() {
        if (!chronological_) {
            size_t ready_index = 0;
            while (ready_index < ready_.size()) execute_node(ready_[ready_index++]);
        } else run_events();
        if (result_.processed_nodes < active_node_count_) throw_cycle_error();
        if (e2e_endpoint_count_ == 0) throw std::runtime_error("Active DAG has no observable business E2E endpoint.");
        result_.e2e_us = e2e_;
        if (mode_ == ReplayMode::Full) {
            graph_.set_e2e_time(e2e_);
            queues_.materialize(graph_, task_order_);
            result_.cpu_queue_count = queues_.queue_count;
            result_.cpu_task_count = queues_.tasks.size();
            for (const auto& task : queues_.tasks) {
                result_.submission_overlap_count += task.submission_overlap_us != 0;
                result_.submission_overlap_total_us = checked_add(result_.submission_overlap_total_us, task.submission_overlap_us, "queue overlap total overflow");
                result_.submission_overlap_max_us = std::max(result_.submission_overlap_max_us, task.submission_overlap_us);
            }
        }
        else if (mode_ == ReplayMode::ControlOnly) graph_.set_control_e2e_time(e2e_);
        else {
            graph_.set_gap_excluded_e2e_time(e2e_);
#ifdef DEBUG
            graph_.set_gap_excluded_critical_path(build_gap_excluded_critical_path());
#endif
        }
        log_success();
        return result_;
    }

private:
    enum class Execution : uint8_t { Pending, Running, Done };
    enum class QueueEventKind { Finish, Arrival, Start, DeferredStart };
    using QueueEvent = std::tuple<uint64_t, QueueEventKind, size_t, uint64_t>;
    struct Worker {
        size_t active = kInvalidNode;
        uint64_t free_us = 0;
        std::queue<size_t> pending;
    };

    void launch_task(size_t task_id) {
        const auto& task = queues_.tasks[task_id];
        const auto ready = std::max(start_time_[task.first], workers_[task.queue].free_us);
        events_.emplace(checked_add(ready, task.ready_delay_us, "queue ready delay overflow"), QueueEventKind::Start, task.first, ready_generation_[task.first]);
    }

    void admit_task(size_t queue) {
        auto& worker = workers_[queue];
        if (worker.active != kInvalidNode || worker.pending.empty()) return;
        worker.active = worker.pending.front();
        worker.pending.pop();
        task_order_[queue].push_back(worker.active);
        if (task_ready_[worker.active]) launch_task(worker.active);
    }

    void make_ready(size_t id) {
        if (!chronological_) { ready_.push_back(id); return; }
        const auto task = queues_.node_task[id];
        if (task != kInvalidNode && queues_.tasks[task].first == id) {
            task_ready_[task] = true;
            if (workers_[queues_.tasks[task].queue].active == task) launch_task(task);
        } else {
            const auto kind = deferred_boundaries_.contains(id) ? QueueEventKind::DeferredStart : QueueEventKind::Start;
            events_.emplace(checked_add(start_time_[id], effective_ready_delay(nodes_[id]), "CPU ready delay overflow"), kind, id, ready_generation_[id]);
        }
    }

    void finish_node(size_t id) {
        execution_[id] = Execution::Done;
        if (id + 1 < storage_.outgoing.offsets.size())
            for (size_t offset = storage_.outgoing.offsets[id]; offset < storage_.outgoing.offsets[id + 1]; ++offset)
                propagate_edge(nodes_[id], storage_.outgoing.destinations[offset], storage_.outgoing.edge_kinds[offset]);
        if (const auto found = added_outgoing_.find(id); found != added_outgoing_.end())
            for (const auto edge_id : found->second) {
                const auto & edge = graph_.edge(edge_id);
                propagate_edge(nodes_[id], edge.dst, edge.kind);
            }
    }

    void run_events() {
        while (!events_.empty()) {
            const auto [time, kind, id, generation] = events_.top();
            events_.pop();
            if (kind == QueueEventKind::Start || kind == QueueEventKind::DeferredStart) {
                if (generation != ready_generation_[id] || storage_.indegree[id] != 0 || execution_[id] != Execution::Pending) continue;
                start_time_[id] = time;
                execute_node(id);
            } else if (kind == QueueEventKind::Arrival) {
                auto& worker = workers_[queues_.tasks[id].queue];
                worker.pending.push(id);
                result_.max_cpu_queue_depth = std::max(result_.max_cpu_queue_depth,
                    worker.pending.size() + static_cast<size_t>(worker.active != kInvalidNode));
                worker.free_us = std::max(worker.free_us, time);
                admit_task(queues_.tasks[id].queue);
            } else {
                finish_node(id);
                const auto submitted = queues_.submitted_task[id];
                if (submitted != kInvalidNode) events_.emplace(time, QueueEventKind::Arrival, submitted, 0);
                const auto task_id = queues_.node_task[id];
                if (task_id != kInvalidNode && queues_.tasks[task_id].last == id) {
                    auto& worker = workers_[queues_.tasks[task_id].queue];
                    worker.active = kInvalidNode;
                    worker.free_us = time;
                    admit_task(queues_.tasks[task_id].queue);
                }
            }
        }
    }

    [[nodiscard]] static uint64_t checked_add(uint64_t left, uint64_t right, const char * message) {
        if (left > std::numeric_limits<uint64_t>::max() - right) throw std::overflow_error(message);
        return left + right;
    }

    void execute_node(size_t node_id) {
        result_.processed_nodes++;
        execution_[node_id] = Execution::Running;
        if (!chronological_) start_time_[node_id] = checked_add(start_time_[node_id], effective_ready_delay(nodes_[node_id]), "DAG CPU ready-delay overflow");
        if (expand_at_start_) {
            expanding_node_ = node_id;
            expand_at_start_(node_id, start_time_[node_id], *this);
            expanding_node_ = kInvalidNode;
        }
        // Expansion can reallocate node storage; acquire this reference afterwards.
        auto & node = graph_.mutable_node(node_id);
        if (cost_at_start_) node.duration = cost_at_start_(node_id, start_time_[node_id], node.duration);
        if (deferred_boundaries_.contains(node_id) && node.duration)
            throw std::logic_error("Deferred observation cannot acquire a service cost");
        const auto completion_time = checked_add(start_time_[node_id], effective_node_duration(node), "DAG simulation timestamp overflow");
        completion_time_[node_id] = completion_time;
        if (mode_ == ReplayMode::Full) {
            node.simulation_start = start_time_[node_id];
            node.completion_time = completion_time;
        }
        if (node.counts_toward_e2e) {
            if (completion_time > e2e_) {
                e2e_ = completion_time;
#ifdef DEBUG
                if (mode_ == ReplayMode::GapExcluded) e2e_endpoint_node_id_ = node_id;
#endif
            }
            e2e_endpoint_count_++;
        }
        if (!chronological_) finish_node(node_id);
        else events_.emplace(completion_time, QueueEventKind::Finish, node_id, 0);
    }

    void propagate_edge(const core::DagNode & source, size_t dst, core::DagEdgeKind kind) {
        const auto delay = effective_edge_delay(source, kind);
        const auto candidate = checked_add(completion_time_[source.id], delay, "DAG simulation edge-delay overflow");
        if (candidate > start_time_[dst]) {
            start_time_[dst] = candidate;
#ifdef DEBUG
            if (mode_ == ReplayMode::GapExcluded) {
                predecessor_[dst] = source.id;
                predecessor_edge_kind_[dst] = kind;
                predecessor_delay_us_[dst] = delay;
            }
#endif
        }
        storage_.indegree[dst]--;
        if (storage_.indegree[dst] == 0) make_ready(dst);
    }

    size_t append(const core::DagSyntheticNodeSpec & spec, BoundaryOrder order, std::optional<size_t> owner) override {
        if (expanding_node_ == kInvalidNode || spec.cpu_task_ready_delay_us)
            throw std::invalid_argument("Future work requires an active expansion and explicit dependencies, not a new native CPU task");
        if (order == BoundaryOrder::AfterConcurrentStarts && (spec.duration || spec.cpu_gap_after))
            throw std::invalid_argument("Deferred boundaries must be zero-cost observations");
        const auto origin = owner.value_or(expanding_node_);
        if (origin >= nodes_.size() || !nodes_[origin].active) throw std::invalid_argument("Future work owner must be an active node");
        const auto rank = nodes_[origin].gpu_id;
        const auto id = graph_.add_synthetic_node(spec);
        if (order == BoundaryOrder::AfterConcurrentStarts) deferred_boundaries_.insert(id);
        graph_.mutable_node(id).gpu_id = rank;
        graph_.mutable_event_for_node(id).pid = graph_.event_for_node(origin).pid;
        storage_.indegree.push_back(0);
        start_time_.push_back(0);
        completion_time_.push_back(0);
        execution_.push_back(Execution::Pending);
        ready_generation_.push_back(0);
        queues_.node_task.push_back(kInvalidNode);
        queues_.submitted_task.push_back(kInvalidNode);
        ++active_node_count_;
        depend(expanding_node_, id, core::DagEdgeKind::Mutation);
        return id;
    }

    size_t append_cpu_task(const core::DagSyntheticNodeSpec & spec, size_t submission, size_t queue_member) override {
        if (expanding_node_ == kInvalidNode || !spec.is_cpu || spec.cpu_gap_after || submission >= nodes_.size()
            || queue_member >= nodes_.size() || !nodes_[submission].active || !nodes_[submission].is_cpu
            || execution_[submission] == Execution::Done || queues_.submitted_task[submission] != kInvalidNode
            || queues_.node_task[queue_member] == kInvalidNode || !spec.cpu_task_ready_delay_us)
            throw std::invalid_argument("New CPU task requires one unfinished submission and a recognized worker");
        const auto queue = queues_.tasks[queues_.node_task[queue_member]].queue;
        if (graph_.node_lane_key(queue_member) != spec.lane_key || nodes_[submission].lane_id == nodes_[queue_member].lane_id
            || nodes_[submission].gpu_id != nodes_[queue_member].gpu_id)
            throw std::invalid_argument("New CPU task must join the declared cross-thread worker on the same rank");
        auto plain = spec;
        plain.cpu_task_ready_delay_us.reset();
        const auto id = append(plain, BoundaryOrder::Ordinary, submission);
        graph_.mutable_node(id).explicit_cpu_task = true;
        graph_.mutable_node(id).cpu_ready_delay_before = *spec.cpu_task_ready_delay_us;
        const auto task = queues_.tasks.size();
        queues_.tasks.push_back({id, id, submission, queue, *spec.cpu_task_ready_delay_us, 0});
        queues_.node_task[id] = task;
        queues_.submitted_task[submission] = task;
        task_ready_.push_back(false);
        connect(submission, id, core::DagEdgeKind::Correlation);
        return id;
    }

    void depend(size_t src, size_t dst, core::DagEdgeKind kind) override {
        if (kind == core::DagEdgeKind::Correlation)
            throw std::invalid_argument("Use append_cpu_task to register a new worker submission");
        connect(src, dst, kind);
    }

    void connect(size_t src, size_t dst, core::DagEdgeKind kind) {
        if (expanding_node_ == kInvalidNode || src >= nodes_.size() || dst >= nodes_.size() || !nodes_[src].active || !nodes_[dst].active
            || src == dst || execution_[dst] != Execution::Pending)
            throw std::invalid_argument("Future dependency must target unstarted work without changing native CPU task identity: src="
                + std::to_string(src) + " dst=" + std::to_string(dst) + " callback=" + std::to_string(expanding_node_)
                + " src_active=" + std::to_string(src < nodes_.size() && nodes_[src].active)
                + " dst_active=" + std::to_string(dst < nodes_.size() && nodes_[dst].active)
                + " src_name=" + (src < nodes_.size() ? graph_.event_for_node(src).name : "missing")
                + " dst_name=" + (dst < nodes_.size() ? graph_.event_for_node(dst).name : "missing")
                + " callback_name=" + (expanding_node_ < nodes_.size() ? graph_.event_for_node(expanding_node_).name : "missing")
                + " dst_state=" + (dst >= execution_.size() ? "missing" : execution_[dst] == Execution::Pending ? "pending"
                    : execution_[dst] == Execution::Running ? "running" : "done"));
        if (queues_.replaces(core::DagEdge{src, dst, kind}))
            throw std::invalid_argument("Native worker ordering is owned by its CPU queue, not a new Sequential dependency");
        // Repeated model declarations must not multiply dependencies.
        if (src + 1 < storage_.outgoing.offsets.size())
            for (size_t i = storage_.outgoing.offsets[src]; i < storage_.outgoing.offsets[src + 1]; ++i)
                if (storage_.outgoing.destinations[i] == dst && storage_.outgoing.edge_kinds[i] == kind) return;
        auto & outgoing = added_outgoing_[src];
        for (const auto id : outgoing) {
            const auto & edge = graph_.edge(id);
            if (edge.dst == dst && edge.kind == kind) return;
        }
        outgoing.push_back(graph_.add_edge(src, dst, kind));
        ++storage_.active_edge_count;
        ++storage_.indegree[dst];
        ++ready_generation_[dst]; // Invalidate any old Start already in the heap.
        const auto task = queues_.node_task[dst];
        if (task != kInvalidNode && queues_.tasks[task].first == dst) task_ready_[task] = false;
        if (execution_[src] == Execution::Done) propagate_edge(nodes_[src], dst, kind);
    }

#ifdef DEBUG
    [[nodiscard]] std::vector<core::DagCriticalPathStep> build_gap_excluded_critical_path() const {
        std::vector<core::DagCriticalPathStep> path;
        for (auto node_id = e2e_endpoint_node_id_; node_id != kInvalidNode; node_id = predecessor_[node_id]) {
            path.push_back(core::DagCriticalPathStep{
                .node_id = node_id,
                .predecessor_node_id = predecessor_[node_id],
                .incoming_edge_kind = predecessor_edge_kind_[node_id],
                .incoming_delay_us = predecessor_delay_us_[node_id],
                .effective_duration_us = effective_node_duration(nodes_[node_id]),
            });
        }
        std::ranges::reverse(path);
        return path;
    }
#endif

    [[nodiscard]] uint64_t scaled_outside_duration(uint64_t current_duration, uint64_t observed_duration, uint64_t overlap) const {
        if (current_duration == 0 || observed_duration == 0 || overlap >= observed_duration) return 0;
        const auto scaled = core::floor_multiply_divide_u64(current_duration, observed_duration - overlap, observed_duration);
        if (!scaled) throw std::overflow_error("control-only duration scaling exceeds uint64 range");
        return *scaled;
    }

    [[nodiscard]] uint64_t effective_ready_delay(const core::DagNode & node) const {
        if (mode_ == ReplayMode::GapExcluded) return 0;
        const auto delay = node.cpu_ready_delay_before;
        if (mode_ != ReplayMode::ControlOnly || delay == 0) return delay;
        const auto start = graph_.event_for_node(node.id).ts;
        return delay - control_exclusions_.overlap_us(node.gpu_id, start - delay, start);
    }

    [[nodiscard]] uint64_t effective_node_duration(const core::DagNode & node) const {
        if (mode_ == ReplayMode::Full) return graph_.cpu_service_node_duration(node.id);
        if (mode_ == ReplayMode::GapExcluded) return graph_.scope_node_owned(node.id) ? node.duration : 0;
        if (mode_ != ReplayMode::ControlOnly || node.kind == core::DagNodeKind::Synthetic || node.duration == 0) return node.duration;
        const auto & event = graph_.event_for_node(node.id);
        const auto event_end = checked_add(event.ts, event.dur, "trace event end exceeds uint64 range");
        const auto overlap = control_exclusions_.overlap_us(node.gpu_id, event.ts, event_end);
        return scaled_outside_duration(node.duration, event.dur, overlap);
    }

    [[nodiscard]] uint64_t effective_edge_delay(const core::DagNode & source, core::DagEdgeKind kind) const {
        const auto current_delay = topological_edge_delay_us(source, kind);
        if (current_delay == 0) return current_delay;
        if (mode_ == ReplayMode::Full) return graph_.cpu_service_gap_duration(source.id);
        if (mode_ == ReplayMode::GapExcluded) return graph_.scope_gap_duration(source.id);
        if (source.original_cpu_gap_after == 0) return current_delay;
        const auto & event = graph_.event_for_node(source.id);
        const auto gap_start = checked_add(event.ts, event.dur, "CPU gap start exceeds uint64 range");
        const auto gap_end = checked_add(gap_start, source.original_cpu_gap_after, "CPU gap end exceeds uint64 range");
        const auto overlap = control_exclusions_.overlap_us(source.gpu_id, gap_start, gap_end);
        return scaled_outside_duration(current_delay, source.original_cpu_gap_after, overlap);
    }

    [[noreturn]] void throw_cycle_error() const {
        const auto error =
            queues_.tasks.empty() ? "Cycle detected in DAG. Simulation aborted." : "DAG or CPU task queue stalled: dependencies conflict with execution order.";
        const auto dynamic_storage = added_outgoing_.empty() ? ActiveDagStorage{} : build_active_dag_storage(graph_, queues_);
        const auto & adjacency = added_outgoing_.empty() ? storage_.outgoing : dynamic_storage.outgoing;
        const auto cycle_nodes = core::detail::find_unresolved_cycle(adjacency.offsets, adjacency.destinations, storage_.indegree);
        auto log = core::Logger::instance().error();
        log << error << " Processed " << result_.processed_nodes << " out of " << active_node_count_ << " active nodes.";
        if (!cycle_nodes.empty()) {
            log << " Cycle nodes:";
            std::ranges::for_each(cycle_nodes, [&](auto node_id) { log << " " << node_id; });
        }
        throw std::runtime_error(error);
    }

    void log_success() const {
        auto & logger = core::Logger::instance();
        if (!logger.enabled(core::Logger::Info)) return;
        const auto label = mode_ == ReplayMode::Full ? "Simulation" : mode_ == ReplayMode::ControlOnly ? "Control-only simulation" : "Gap-excluded simulation";
        logger.info() << label << " completed. End-to-End time: " << e2e_
                      << " us | nodes: " << result_.processed_nodes << " edges: " << storage_.active_edge_count;
    }

    core::DagGraph & graph_;
    const std::vector<core::DagNode> & nodes_;
    size_t active_node_count_ = 0;
    detail::CpuTaskQueues queues_;
    ActiveDagStorage storage_;
    std::vector<uint64_t> start_time_;
    std::vector<uint64_t> completion_time_;
    std::vector<Worker> workers_;
    std::vector<bool> task_ready_;
    std::vector<std::vector<size_t>> task_order_;
    std::priority_queue<QueueEvent, std::vector<QueueEvent>, std::greater<QueueEvent>> events_;
    std::vector<size_t> ready_;
    SimulationResult result_;
    uint64_t e2e_ = 0;
    size_t e2e_endpoint_count_ = 0;
    ReplayMode mode_ = ReplayMode::Full;
    ControlExclusionIndex control_exclusions_;
    NodeCostAtStart cost_at_start_;
    ExpandAtStart expand_at_start_;
    std::vector<Execution> execution_;
    std::vector<uint64_t> ready_generation_;
    std::unordered_set<size_t> deferred_boundaries_;
    std::unordered_map<size_t, std::vector<size_t>> added_outgoing_;
    size_t expanding_node_ = kInvalidNode;
    bool chronological_ = false;
#ifdef DEBUG
    size_t e2e_endpoint_node_id_ = kInvalidNode;
    std::vector<size_t> predecessor_;
    std::vector<core::DagEdgeKind> predecessor_edge_kind_;
    std::vector<uint64_t> predecessor_delay_us_;
#endif
};

} // namespace topological_simulator_detail

SimulationResult run_topological_simulation(core::DagGraph & graph) {
    return topological_simulator_detail::TopologicalSimulation(graph, topological_simulator_detail::ReplayMode::Full).run();
}

SimulationResult run_control_topological_simulation(core::DagGraph & graph) {
    return topological_simulator_detail::TopologicalSimulation(graph, topological_simulator_detail::ReplayMode::ControlOnly).run();
}

SimulationResult run_topological_simulation(core::DagGraph & graph, const NodeCostAtStart & cost_at_start) {
    return topological_simulator_detail::TopologicalSimulation(graph, topological_simulator_detail::ReplayMode::Full, cost_at_start).run();
}

SimulationResult run_topological_simulation(core::DagGraph & graph, const NodeCostAtStart & cost_at_start, const ExpandAtStart & expand_at_start) {
    return topological_simulator_detail::TopologicalSimulation(graph, topological_simulator_detail::ReplayMode::Full, cost_at_start, expand_at_start).run();
}

SimulationResult run_gap_excluded_topological_simulation(core::DagGraph & graph) {
    return topological_simulator_detail::TopologicalSimulation(graph, topological_simulator_detail::ReplayMode::GapExcluded).run();
}

} // namespace markov::trace_graph::simulation
