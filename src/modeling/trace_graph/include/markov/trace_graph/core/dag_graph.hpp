/**
 * @file
 * @brief TraceGraph 的核心 DAG 数据结构和基础 mutation API。
 */
#pragma once

#include "markov/trace_graph/core/trace_event.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace markov::trace_graph::core {

/**
 * @brief DAG 边的语义来源。
 *
 * 不同来源的边在拓扑仿真中都表示“dst 必须等 src 完成”，但 summary/debug 会按 kind 拆分。
 */
enum class DagEdgeKind { Sequential, Stream, Correlation, Sync, HCCL, HiCache, Mutation };

/** @brief DAG 中的一条硬依赖边。 */
struct DagEdge {
    /** @brief src/dst 都是 DagGraph::nodes_ 中的 node id，不是 TraceEvent::index。 */
    size_t src = 0;
    size_t dst = 0;
    DagEdgeKind kind = DagEdgeKind::Sequential;
};

/**
 * @brief 一条可执行 duration event 在建模图中的实例。
 *
 * 当前实现保持“一条 TraceEvent 对应一个 DagNode”的基础关系；后续子模块可以
 * 新增或修改节点，但需要继续维护 event_index / attrs 中的可追溯信息。
 */
struct DagNode {
    size_t id = 0;

    /** @brief 指向 events_ 中的原始事件；merge 多 rank 时会随 event_offset 重写。 */
    size_t event_index = 0;

    /** @brief 单输入 trace 构图时由 CLI 输入序号生成；merge 后主要用于输出和 HCCL 分组。 */
    int gpu_id = 0;

    /** @brief CPU 节点和 device 节点的顺序约束不同：CPU 使用 Sequential，device 使用 Stream。 */
    bool is_cpu = true;

    /** @brief 逻辑执行 lane；CPU 当前可以被合并为 CPU_MERGED，device lane 通常来自 tid/stream。 */
    std::string lane_key;

    /** @brief 当前用于仿真的耗时；子模块修改 duration 时必须同步 attrs["time"]。 */
    uint64_t duration = 0;

    /** @brief 原始耗时，用于 what-if 缩放和 debug 对比。 */
    uint64_t original_duration = 0;

    /** @brief 拓扑仿真后写入的相对开始/结束时间。 */
    uint64_t simulation_start = 0;
    uint64_t completion_time = 0;

    /** @brief 松散元数据区，承载历史字段和后续子模块 mutation metadata。 */
    std::unordered_map<std::string, std::string> attrs;
};

/**
 * @brief 持有归一化事件、DAG 节点、DAG 边和仿真统计。
 *
 * DagGraph 不负责解释 trace 语义；语义边由 DagBuilder 或 SimulationModule 添加。
 */
class DagGraph {
public:
    explicit DagGraph(std::vector<TraceEvent> events = {}, int gpu_id = 0);

    /** @brief 创建节点时只设置基础属性，不自动加任何依赖边。 */
    size_t add_node(size_t event_index, bool is_cpu, const std::string & lane_key);

    /** @brief 预留节点和边容量，用于大 trace 构图减少重复扩容。 */
    void reserve(size_t node_capacity, size_t edge_capacity);

    /** @brief 添加一条硬依赖边；当前不做去重，调用方需要避免重复边导致 indegree 膨胀。 */
    void add_edge(size_t src, size_t dst, DagEdgeKind kind);

    /** @brief 原始事件的只读视图。 */
    [[nodiscard]] const std::vector<TraceEvent> & events() const { return events_; }

    /** @brief 原始事件的可变视图；只允许 normalizer/模块在明确边界内修改。 */
    std::vector<TraceEvent> & mutable_events() { return events_; }

    /** @brief DAG 节点只读视图。 */
    [[nodiscard]] const std::vector<DagNode> & nodes() const { return nodes_; }

    /** @brief DAG 边只读视图。 */
    [[nodiscard]] const std::vector<DagEdge> & edges() const { return edges_; }

    /** @brief 按 trace event index 读取事件。 */
    [[nodiscard]] const TraceEvent & event(size_t event_index) const;

    /** @brief 按 trace event index 获取可变事件。 */
    TraceEvent & mutable_event(size_t event_index);

    /** @brief 读取 node_id 对应的原始事件。 */
    [[nodiscard]] const TraceEvent & event_for_node(size_t node_id) const;

    /** @brief 获取 node_id 对应的可变原始事件。 */
    TraceEvent & mutable_event_for_node(size_t node_id);

    /** @brief 读取 DAG node。 */
    [[nodiscard]] const DagNode & node(size_t node_id) const;

    /** @brief 获取可变 DAG node。 */
    DagNode & mutable_node(size_t node_id);

    /** @brief 写入节点属性；调用方负责保证属性语义一致。 */
    void set_node_attr(size_t node_id, const std::string & key, const std::string & value);

    /** @brief 修改节点耗时时同时更新 attrs["time"]，保证拓扑仿真和 debug 输出读到一致值。 */
    void set_node_duration(size_t node_id, uint64_t duration);

    /** @brief 读取节点属性，缺失时返回 fallback。 */
    [[nodiscard]] std::string node_attr(size_t node_id, const std::string & key, const std::string & fallback = "") const;

    /** @brief CPU lane 集合用于决定 lane 顺序边类型，以及输出时是否合并 pid/tid。 */
    void set_cpu_lane(const std::string & lane_key);

    /** @brief 判断 lane 是否被登记为 CPU lane。 */
    [[nodiscard]] bool is_cpu_lane(const std::string & lane_key) const;

    /** @brief 所有已登记 CPU lane 的只读集合。 */
    [[nodiscard]] const std::unordered_set<std::string> & cpu_lanes() const { return cpu_lanes_; }

    /** @brief 当前 DAG node 数。 */
    [[nodiscard]] size_t node_count() const { return nodes_.size(); }

    /** @brief 当前 DAG edge 数。 */
    [[nodiscard]] size_t edge_count() const { return edges_.size(); }

    /** @brief reader 解析到的原始记录数，包含被过滤事件。 */
    [[nodiscard]] size_t parsed_record_count() const { return parsed_record_count_; }

    /** @brief 记录 reader 解析到的原始记录数。 */
    void set_parsed_record_count(size_t value) { parsed_record_count_ = value; }

    /** @brief 原始 trace 的真实端到端时间窗口。 */
    [[nodiscard]] uint64_t real_e2e_time() const { return real_e2e_time_; }

    /** @brief 设置原始 trace 的真实端到端时间窗口。 */
    void set_real_e2e_time(uint64_t value) { real_e2e_time_ = value; }

    /** @brief 按边类型统计 DAG edge 数。 */
    [[nodiscard]] std::unordered_map<std::string, size_t> edge_counts_by_kind() const;

    /** @brief trace 中识别出的 device id；未知时为默认值。 */
    [[nodiscard]] int gpu_id() const { return gpu_id_; }

    /** @brief 写入拓扑仿真得到的端到端时间。 */
    void set_e2e_time(uint64_t value) { e2e_time_ = value; }

    /** @brief 读取拓扑仿真得到的端到端时间。 */
    [[nodiscard]] uint64_t e2e_time() const { return e2e_time_; }

    /**
     * @brief 合并多输入 trace 构出的 per-rank graph。
     *
     * 目前只在 merge 阶段建立 HCCL 跨 rank 约束；其他跨 rank 语义需要后续子模块补充。
     */
    static DagGraph merge(std::vector<DagGraph> graphs);

private:
    /** @brief 保存所有被 parser 接受的 duration event；不包含 metadata / flow event。 */
    std::vector<TraceEvent> events_;
    std::vector<DagNode> nodes_;
    std::vector<DagEdge> edges_;

    /** @brief CPU lane 被单独记录，是因为 lane_key 本身只是字符串，不能从字符串判断资源类型。 */
    std::unordered_set<std::string> cpu_lanes_;
    int gpu_id_ = 0;

    /** @brief e2e_time_ 是拓扑仿真结果；real_e2e_time_ 是输入 trace 自身的真实时间窗口。 */
    uint64_t e2e_time_ = 0;
    uint64_t real_e2e_time_ = 0;

    /** @brief parser 接受的 duration event 数；命名沿用历史 summary 字段。 */
    size_t parsed_record_count_ = 0;
};

} // namespace markov::trace_graph::core
