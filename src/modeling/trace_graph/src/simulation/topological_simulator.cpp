/**
 * @file
 * @brief Compact-CSR topological simulator implementation.
 */
#include "markov/trace_graph/simulation/topological_simulator.hpp"

#include "markov/trace_graph/core/logger.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
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
    explicit TopologicalSimulation(core::DagGraph & graph)
        : graph_(graph),
          nodes_(graph.nodes()),
          active_node_count_(graph.active_node_count()),
          storage_(build_active_dag_storage(graph)),
          start_time_(graph.node_count(), 0) {
        ready_.reserve(active_node_count_);
        for (const auto & node : nodes_) {
            if (node.active && storage_.indegree[node.id] == 0) ready_.push_back(node.id);
        }
    }

    [[nodiscard]] SimulationResult run() {
        size_t ready_index = 0;
        while (ready_index < ready_.size()) execute_node(ready_[ready_index++]);
        if (result_.processed_nodes < active_node_count_) throw_cycle_error();
        result_.e2e_us = e2e_;
        graph_.set_e2e_time(e2e_);
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
        const auto completion_time = checked_add(start_time_[node_id], node.duration, "DAG simulation timestamp overflow");
        node.simulation_start = start_time_[node_id];
        node.completion_time = completion_time;
        e2e_ = std::max(e2e_, completion_time);
        for (size_t offset = storage_.outgoing.offsets[node_id]; offset < storage_.outgoing.offsets[node_id + 1]; ++offset) { propagate_edge(node, offset); }
    }

    void propagate_edge(const core::DagNode & source, size_t offset) {
        const auto dst = storage_.outgoing.destinations[offset];
        const auto delay = topological_edge_delay_us(source, storage_.outgoing.edge_kinds[offset]);
        start_time_[dst] = std::max(start_time_[dst], checked_add(source.completion_time, delay, "DAG simulation edge-delay overflow"));
        storage_.indegree[dst]--;
        if (storage_.indegree[dst] == 0) ready_.push_back(dst);
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
        logger.info() << "Simulation completed. End-to-End time: " << e2e_ << " us | nodes: " << result_.processed_nodes
                      << " edges: " << storage_.active_edge_count;
    }

    core::DagGraph & graph_;
    const std::vector<core::DagNode> & nodes_;
    size_t active_node_count_ = 0;
    ActiveDagStorage storage_;
    std::vector<uint64_t> start_time_;
    std::vector<size_t> ready_;
    SimulationResult result_;
    uint64_t e2e_ = 0;
};

} // namespace topological_simulator_detail

SimulationResult run_topological_simulation(core::DagGraph & graph) { return topological_simulator_detail::TopologicalSimulation(graph).run(); }

} // namespace markov::trace_graph::simulation
