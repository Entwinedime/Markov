/**
 * @file
 * @brief Compact-CSR topological simulator implementation.
 */
#include "markov/trace_graph/simulation/topological_simulator.hpp"

#include "markov/trace_graph/core/logger.hpp"
#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace markov::trace_graph::simulation {

namespace topological_simulator_detail {

constexpr size_t kInvalidNode = std::numeric_limits<size_t>::max();

struct DfsFrame {
    size_t node_id = kInvalidNode;
    size_t next_offset = 0;
};

struct CsrAdjacency {
    std::vector<size_t> offsets;
    std::vector<size_t> destinations;
    std::vector<core::DagEdgeKind> edge_kinds;
};

class CycleFinder {
public:
    CycleFinder(const CsrAdjacency & outgoing, const std::vector<size_t> & indegree)
        : outgoing_(outgoing),
          indegree_(indegree),
          node_count_(outgoing.offsets.empty() ? 0 : outgoing.offsets.size() - 1),
          visit_state_(node_count_, 0),
          path_position_(node_count_, kInvalidNode) {}

    [[nodiscard]] std::vector<size_t> run() {
        for (size_t start = 0; start < node_count_; ++start) {
            if (!is_unresolved(start) || visit_state_[start] != 0) continue;
            auto cycle = search_from(start);
            if (!cycle.empty()) return cycle;
        }
        return {};
    }

private:
    [[nodiscard]] bool is_unresolved(size_t node_id) const { return node_id < node_count_ && indegree_[node_id] > 0; }

    void push_node(size_t node_id) {
        visit_state_[node_id] = 1;
        path_position_[node_id] = path_stack_.size();
        path_stack_.push_back(node_id);
        dfs_stack_.push_back(DfsFrame{ .node_id = node_id, .next_offset = outgoing_.offsets[node_id] });
    }

    [[nodiscard]] std::vector<size_t> cycle_ending_at(size_t node_id) const {
        const auto position = path_position_[node_id];
        if (position == kInvalidNode || position >= path_stack_.size()) return { node_id };
        return std::vector<size_t>(path_stack_.begin() + static_cast<std::ptrdiff_t>(position), path_stack_.end());
    }

    [[nodiscard]] std::optional<std::vector<size_t>> advance(DfsFrame & frame, bool & descended) {
        while (frame.next_offset < outgoing_.offsets[frame.node_id + 1]) {
            const auto dst = outgoing_.destinations[frame.next_offset++];
            if (!is_unresolved(dst)) continue;
            if (visit_state_[dst] == 0) {
                push_node(dst);
                descended = true;
                return std::nullopt;
            }
            if (visit_state_[dst] == 1) return cycle_ending_at(dst);
        }
        return std::nullopt;
    }

    void finish_top_frame() {
        const auto done = dfs_stack_.back().node_id;
        dfs_stack_.pop_back();
        visit_state_[done] = 2;
        path_position_[done] = kInvalidNode;
        if (!path_stack_.empty()) path_stack_.pop_back();
    }

    [[nodiscard]] std::vector<size_t> search_from(size_t start) {
        dfs_stack_.clear();
        path_stack_.clear();
        push_node(start);
        while (!dfs_stack_.empty()) {
            bool descended = false;
            auto cycle = advance(dfs_stack_.back(), descended);
            if (cycle) return std::move(*cycle);
            if (!descended) finish_top_frame();
        }
        return {};
    }

    const CsrAdjacency & outgoing_;
    const std::vector<size_t> & indegree_;
    size_t node_count_ = 0;
    std::vector<int> visit_state_;
    std::vector<size_t> path_stack_;
    std::vector<size_t> path_position_;
    std::vector<DfsFrame> dfs_stack_;
};

std::vector<size_t> find_cycle_nodes(const CsrAdjacency & outgoing, const std::vector<size_t> & indegree) { return CycleFinder(outgoing, indegree).run(); }

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

enum class ReplayMode : std::uint8_t { Full, ControlOnly };

ActiveDagStorage build_active_dag_storage(const core::DagGraph & graph) {
    ActiveDagStorage storage;
    const auto node_count = graph.node_count();
    const auto & nodes = graph.nodes();
    storage.indegree.resize(node_count, 0);
    storage.outgoing.offsets.resize(node_count + 1, 0);
    for (const auto & edge : graph.edges()) {
        if (!edge.active) continue;
        if (edge.src >= node_count || edge.dst >= node_count) throw std::runtime_error("Invalid active DAG: active edge endpoint is out of range");
        if (!nodes[edge.src].active || !nodes[edge.dst].active) throw std::runtime_error("Invalid active DAG: active edge references a disabled node");
        storage.outgoing.offsets[edge.src + 1]++;
        storage.indegree[edge.dst]++;
        storage.active_edge_count++;
    }

    for (size_t node_id = 0; node_id < node_count; ++node_id) storage.outgoing.offsets[node_id + 1] += storage.outgoing.offsets[node_id];
    storage.outgoing.destinations.resize(storage.active_edge_count);
    storage.outgoing.edge_kinds.resize(storage.active_edge_count, core::DagEdgeKind::Sequential);
    auto cursor = storage.outgoing.offsets;
    for (const auto & edge : graph.edges()) {
        if (!edge.active) continue;
        const auto offset = cursor[edge.src]++;
        storage.outgoing.destinations[offset] = edge.dst;
        storage.outgoing.edge_kinds[offset] = edge.kind;
    }
    return storage;
}

class TopologicalSimulation {
public:
    explicit TopologicalSimulation(core::DagGraph & graph, ReplayMode mode)
        : graph_(graph),
          nodes_(graph.nodes()),
          active_node_count_(graph.active_node_count()),
          storage_(build_active_dag_storage(graph)),
          start_time_(graph.node_count(), 0),
          completion_time_(graph.node_count(), 0),
          mode_(mode),
          control_exclusions_(graph) {
        ready_.reserve(active_node_count_);
        for (const auto & node : nodes_) {
            if (node.active && storage_.indegree[node.id] == 0) ready_.push_back(node.id);
        }
    }

    [[nodiscard]] SimulationResult run() {
        size_t ready_index = 0;
        while (ready_index < ready_.size()) execute_node(ready_[ready_index++]);
        if (result_.processed_nodes < active_node_count_) throw_cycle_error();
        if (e2e_endpoint_count_ == 0) throw std::runtime_error("Active DAG has no observable business E2E endpoint.");
        result_.e2e_us = e2e_;
        if (mode_ == ReplayMode::Full) graph_.set_e2e_time(e2e_);
        else graph_.set_control_e2e_time(e2e_);
        log_success();
        return result_;
    }

private:
    [[nodiscard]] static uint64_t checked_add(uint64_t left, uint64_t right, const char * message) {
        if (left > std::numeric_limits<uint64_t>::max() - right) throw std::overflow_error(message);
        return left + right;
    }

    void execute_node(size_t node_id) {
        result_.processed_nodes++;
        auto & node = graph_.mutable_node(node_id);
        const auto completion_time = checked_add(start_time_[node_id], effective_node_duration(node), "DAG simulation timestamp overflow");
        completion_time_[node_id] = completion_time;
        if (mode_ == ReplayMode::Full) {
            node.simulation_start = start_time_[node_id];
            node.completion_time = completion_time;
        }
        if (node.counts_toward_e2e) {
            e2e_ = std::max(e2e_, completion_time);
            e2e_endpoint_count_++;
        }
        for (size_t offset = storage_.outgoing.offsets[node_id]; offset < storage_.outgoing.offsets[node_id + 1]; ++offset) { propagate_edge(node, offset); }
    }

    void propagate_edge(const core::DagNode & source, size_t offset) {
        const auto dst = storage_.outgoing.destinations[offset];
        const auto delay = effective_edge_delay(source, storage_.outgoing.edge_kinds[offset]);
        start_time_[dst] = std::max(start_time_[dst], checked_add(completion_time_[source.id], delay, "DAG simulation edge-delay overflow"));
        storage_.indegree[dst]--;
        if (storage_.indegree[dst] == 0) ready_.push_back(dst);
    }

    [[nodiscard]] uint64_t scaled_outside_duration(uint64_t current_duration, uint64_t observed_duration, uint64_t overlap) const {
        if (current_duration == 0 || observed_duration == 0 || overlap >= observed_duration) return 0;
        const auto scaled = core::floor_multiply_divide_u64(current_duration, observed_duration - overlap, observed_duration);
        if (!scaled) throw std::overflow_error("control-only duration scaling exceeds uint64 range");
        return *scaled;
    }

    [[nodiscard]] uint64_t effective_node_duration(const core::DagNode & node) const {
        if (mode_ == ReplayMode::Full || node.kind == core::DagNodeKind::Synthetic || node.duration == 0) return node.duration;
        const auto & event = graph_.event_for_node(node.id);
        const auto event_end = checked_add(event.ts, event.dur, "trace event end exceeds uint64 range");
        const auto overlap = control_exclusions_.overlap_us(node.gpu_id, event.ts, event_end);
        return scaled_outside_duration(node.duration, event.dur, overlap);
    }

    [[nodiscard]] uint64_t effective_edge_delay(const core::DagNode & source, core::DagEdgeKind kind) const {
        const auto current_delay = topological_edge_delay_us(source, kind);
        if (mode_ == ReplayMode::Full || current_delay == 0 || source.original_cpu_gap_after == 0) return current_delay;
        const auto & event = graph_.event_for_node(source.id);
        const auto gap_start = checked_add(event.ts, event.dur, "CPU gap start exceeds uint64 range");
        const auto gap_end = checked_add(gap_start, source.original_cpu_gap_after, "CPU gap end exceeds uint64 range");
        const auto overlap = control_exclusions_.overlap_us(source.gpu_id, gap_start, gap_end);
        return scaled_outside_duration(current_delay, source.original_cpu_gap_after, overlap);
    }

    [[noreturn]] void throw_cycle_error() const {
        constexpr auto error = "Cycle detected in DAG. Simulation aborted.";
        const auto cycle_nodes = find_cycle_nodes(storage_.outgoing, storage_.indegree);
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
        logger.info() << (mode_ == ReplayMode::Full ? "Simulation" : "Control-only simulation") << " completed. End-to-End time: " << e2e_
                      << " us | nodes: " << result_.processed_nodes << " edges: " << storage_.active_edge_count;
    }

    core::DagGraph & graph_;
    const std::vector<core::DagNode> & nodes_;
    size_t active_node_count_ = 0;
    ActiveDagStorage storage_;
    std::vector<uint64_t> start_time_;
    std::vector<uint64_t> completion_time_;
    std::vector<size_t> ready_;
    SimulationResult result_;
    uint64_t e2e_ = 0;
    size_t e2e_endpoint_count_ = 0;
    ReplayMode mode_ = ReplayMode::Full;
    ControlExclusionIndex control_exclusions_;
};

} // namespace topological_simulator_detail

SimulationResult run_topological_simulation(core::DagGraph & graph) {
    return topological_simulator_detail::TopologicalSimulation(graph, topological_simulator_detail::ReplayMode::Full).run();
}

SimulationResult run_control_topological_simulation(core::DagGraph & graph) {
    return topological_simulator_detail::TopologicalSimulation(graph, topological_simulator_detail::ReplayMode::ControlOnly).run();
}

} // namespace markov::trace_graph::simulation
