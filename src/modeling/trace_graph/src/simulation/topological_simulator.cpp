/**
 * @file
 * @brief TraceGraph DAG 的拓扑仿真执行器实现。
 */
#include "markov/trace_graph/simulation/topological_simulator.hpp"

#include "markov/trace_graph/core/logger.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <queue>
#include <ranges>
#include <stdexcept>
#include <vector>

namespace markov::trace_graph::simulation {

namespace topological_simulator_detail {

constexpr size_t INVALID_NODE = std::numeric_limits<size_t>::max();

struct DfsFrame {
    size_t node_id = INVALID_NODE;
    size_t next_edge_index = 0;
};

/**
 * @brief 读取仿真阶段使用的节点耗时字段。
 *
 * 子模块可能直接修改 attrs["time"]，也可能通过 DagGraph::set_node_duration 更新。
 * 仿真阶段优先读取 attrs["time"]，使 DAG patch 与基础节点 duration 使用同一入口。
 */
uint64_t read_u64_attr(const core::DagNode & node, const std::string & key, uint64_t fallback = 0) {
    const auto it = node.attrs.find(key);
    if (it == node.attrs.end()) return fallback;
    try {
        return std::stoull(it->second);
    }
    catch (...) {
        return fallback;
    }
}

std::vector<size_t> find_cycle_nodes(const std::vector<std::vector<size_t>> & outgoing, const std::vector<int> & indegree) {
    const size_t node_count = outgoing.size();
    std::vector<int> visit_state(node_count, 0);
    std::vector<size_t> path_stack;
    std::vector<size_t> path_position(node_count, INVALID_NODE);
    std::vector<DfsFrame> dfs_stack;

    for (size_t start = 0; start < node_count; ++start) {
        if (indegree[start] <= 0 || visit_state[start] != 0) continue;

        dfs_stack.clear();
        path_stack.clear();
        dfs_stack.push_back(DfsFrame{ .node_id = start, .next_edge_index = 0 });
        visit_state[start] = 1;
        path_position[start] = path_stack.size();
        path_stack.push_back(start);

        while (!dfs_stack.empty()) {
            auto & frame = dfs_stack.back();
            const auto & edges = outgoing[frame.node_id];
            bool descended = false;

            while (frame.next_edge_index < edges.size()) {
                const size_t dst = edges[frame.next_edge_index++];
                if (dst >= node_count || indegree[dst] <= 0) continue;

                if (visit_state[dst] == 0) {
                    visit_state[dst] = 1;
                    path_position[dst] = path_stack.size();
                    path_stack.push_back(dst);
                    dfs_stack.push_back(DfsFrame{ .node_id = dst, .next_edge_index = 0 });
                    descended = true;
                    break;
                }
                if (visit_state[dst] == 1) {
                    const auto position = path_position[dst];
                    if (position != INVALID_NODE && position < path_stack.size()) {
                        return std::vector<size_t>(path_stack.begin() + static_cast<std::ptrdiff_t>(position), path_stack.end());
                    }
                    return { dst };
                }
            }
            if (descended) continue;

            const size_t done = frame.node_id;
            dfs_stack.pop_back();
            visit_state[done] = 2;
            path_position[done] = INVALID_NODE;
            if (!path_stack.empty()) path_stack.pop_back();
        }
    }
    return {};
}

} // namespace topological_simulator_detail

using topological_simulator_detail::INVALID_NODE;
using topological_simulator_detail::find_cycle_nodes;
using topological_simulator_detail::read_u64_attr;

SimulationResult run_topological_simulation(core::DagGraph & graph) {
    SimulationResult result;
    const size_t node_count = graph.node_count();

    /**
     * @brief 为 Kahn 拓扑排序构建邻接表和入度。
     *
     * 这里不预先压缩重复边；如果上游重复添加同一条边，indegree 会重复增加，
     * 直到对应重复边都被处理完。
     */
    std::vector<std::vector<size_t>> outgoing(node_count);
    std::vector<int> indegree(node_count, 0);
    std::vector<uint64_t> complete_time(node_count, 0);
    std::vector<size_t> critical_pred(node_count, INVALID_NODE);
    std::queue<size_t> ready;

    std::ranges::for_each(graph.edges(), [&](const auto & edge) {
        if (edge.src >= node_count || edge.dst >= node_count) return;
        outgoing[edge.src].push_back(edge.dst);
        indegree[edge.dst]++;
    });
    std::ranges::for_each(graph.nodes(), [&](const auto & node) {
        if (indegree[node.id] == 0) ready.push(node.id);
    });

    uint64_t e2e = 0;
    while (!ready.empty()) {
        auto node_id = ready.front();
        ready.pop();
        result.processed_nodes++;

        auto & node = graph.mutable_node(node_id);
        /**
         * @brief 当前节点自身耗时优先从 attrs["time"] 读取。
         *
         * node.duration 与 attrs["time"] 理论上应一致；attrs 优先级用于承接当前子模块
         * 对节点耗时的显式 patch。
         */
        uint64_t node_time = node.duration;
        auto attr_time = read_u64_attr(node, "time", node.duration);
        if (attr_time != node.duration) node_time = attr_time;
        complete_time[node_id] = node_time;

        /**
         * @brief critical_pred 保存当前已知完成时间最大的前驱。
         *
         * DAG edge 都是 hard dependency，所以节点开始时间由最大完成前驱决定。
         */
        auto pred = critical_pred[node_id];
        if (pred != INVALID_NODE) complete_time[node_id] += complete_time[pred];

        /**
         * @brief CPU 顺序间隔计入关键路径上前驱节点之后。
         */
        if (pred != INVALID_NODE) {
            const auto & pred_node = graph.node(pred);
            if (pred_node.attrs.contains("cpuinterval")) {
                auto interval = read_u64_attr(pred_node, "cpuinterval", 0);
                if (interval <= 1'000'000'000'000ull) complete_time[node_id] += interval;
            }
        }

        node.simulation_start = complete_time[node_id] >= node_time ? complete_time[node_id] - node_time : 0;
        node.completion_time = complete_time[node_id];
        if (complete_time[node_id] > e2e) e2e = complete_time[node_id];

        for (size_t dst : outgoing[node_id]) {
            /**
             * @brief 如果多个前驱都指向同一 dst，只保留完成时间最大的那个作为 critical predecessor。
             */
            if (critical_pred[dst] == INVALID_NODE || complete_time[node_id] > complete_time[critical_pred[dst]]) critical_pred[dst] = node_id;
            indegree[dst]--;
            if (indegree[dst] == 0) ready.push(dst);
        }
    }

    if (result.processed_nodes < node_count) {
        result.cycle_detected = true;
        result.error = "Cycle detected in DAG. Simulation aborted.";

        result.cycle_nodes = find_cycle_nodes(outgoing, indegree);

        auto log = core::Logger::instance().error();
        log << result.error << " Processed " << result.processed_nodes << " out of " << node_count << " nodes.";
        if (!result.cycle_nodes.empty()) {
            log << " Cycle nodes:";
            std::ranges::for_each(result.cycle_nodes, [&](auto node_id) { log << " " << node_id; });
        }
        throw std::runtime_error(result.error);
    }

    result.e2e_ns = e2e;
    graph.set_e2e_time(e2e);
    core::Logger::instance().info() << "Simulation completed. End-to-End time: " << e2e << " ns | nodes: " << result.processed_nodes
                                    << " edges: " << graph.edge_count();
    return result;
}

} // namespace markov::trace_graph::simulation
