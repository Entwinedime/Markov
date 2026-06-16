#include "trace_graph/simulation/topological_simulator.hpp"

#include "trace_graph/core/logger.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <queue>
#include <ranges>
#include <stdexcept>
#include <vector>

namespace TraceGraph {

namespace {

constexpr size_t INVALID_NODE = std::numeric_limits<size_t>::max();

/**
 * @brief 读取仿真阶段使用的节点耗时字段。
 *
 * 子模块可能只更新 attrs["time"]，也可能通过 DagGraph::set_node_duration 更新。
 * 仿真阶段优先读取 attrs["time"]，保证兼容老版字段。
 */
uint64_t read_u64_attr(const DagNode & node, const std::string & key, uint64_t fallback = 0) {
    const auto it = node.attrs.find(key);
    if (it == node.attrs.end()) return fallback;
    try {
        return std::stoull(it->second);
    }
    catch (...) {
        return fallback;
    }
}

} // namespace

SimulationResult run_topological_simulation(DagGraph & graph) {
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
         * node.duration 与 attrs["time"] 理论上应一致；这里保留 attrs 优先级，是为了兼容历史
         * 子模块直接写 attrs 的行为。
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
         * @brief 老版 TraceGraph 会把 CPU 顺序间隔计入关键路径上前驱节点之后。
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
        node.attrs["simulationtime"] = std::to_string(node.simulation_start);
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

        /**
         * @brief 拓扑排序失败时再做一次 DFS，尽量输出一段具体 cycle node id，方便定位错误边。
         */
        std::vector<int> visit_state(node_count, 0);
        std::vector<size_t> path_stack;
        std::function<bool(size_t)> dfs = [&](size_t node_id) {
            visit_state[node_id] = 1;
            path_stack.push_back(node_id);
            for (size_t dst : outgoing[node_id]) {
                if (indegree[dst] == 0) continue;
                if (visit_state[dst] == 0) {
                    if (dfs(dst)) return true;
                }
                else if (visit_state[dst] == 1) {
                    auto it = std::ranges::find(path_stack, dst);
                    if (it != path_stack.end()) result.cycle_nodes.assign(it, path_stack.end());
                    return true;
                }
            }
            path_stack.pop_back();
            visit_state[node_id] = 2;
            return false;
        };

        for (const auto node_id : std::views::iota(size_t{ 0 }, node_count)) {
            if (indegree[node_id] > 0 && visit_state[node_id] == 0 && dfs(node_id)) break;
        }

        auto log = Logger::instance().error();
        log << result.error << " Processed " << result.processed_nodes << " out of " << node_count << " nodes.";
        if (!result.cycle_nodes.empty()) {
            log << " Cycle nodes:";
            std::ranges::for_each(result.cycle_nodes, [&](auto node_id) { log << " " << node_id; });
        }
        throw std::runtime_error(result.error);
    }

    result.e2e_ns = e2e;
    graph.set_e2e_time(e2e);
    Logger::instance().info() << "Simulation completed. End-to-End time: " << e2e << " ns | nodes: " << result.processed_nodes
                              << " edges: " << graph.edge_count();
    return result;
}

} // namespace TraceGraph
