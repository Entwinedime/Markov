/**
 * @file
 * @brief Chrome trace event 到基础执行 DAG 的构建器。
 */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace markov::trace_graph::core {

/**
 * @brief 把一组 TraceEvent 转换成可仿真的 base DAG。
 *
 * DagBuilder 只负责 faithful replay 的基础因果边：
 * - 同 CPU lane 的顺序边；
 * - 同 device stream/lane 的顺序边；
 * - runtime launch 到 kernel 的 correlation 边；
 * - event/stream sync 边；
 * - 少量老版 TraceGraph 兼容的 notify/model_execute 边。
 *
 * what-if 子模块不应该把额外策略逻辑塞进 DagBuilder，而应该在 SimulationModule 中修改 DAG。
 */
class DagBuilder {
public:
    /** @brief 构建单 rank / 单输入 trace 的 base DAG。 */
    [[nodiscard]] DagGraph build(std::vector<TraceEvent> events, int gpu_id) const;

private:
    /**
     * @brief 单次构图中的临时索引。
     *
     * 构图过程中反复需要按不同 key 查节点；BuildIndex 只在一次 build 内有效，
     * 不能跨 graph 或跨输入 trace 复用。
     */
    struct BuildIndex {
        /** @brief lane -> node ids，用于同 lane 顺序边和 stream sync 查找。 */
        std::unordered_map<std::string, std::vector<size_t>> lane_to_nodes;
        /** @brief correlation_id / connection_id 是 runtime 与 device event 的主要关联证据。 */
        std::unordered_map<std::string, std::vector<size_t>> correlation_to_nodes;
        std::unordered_map<std::string, std::vector<size_t>> connection_to_nodes;
        /** @brief 只收集可确认 event id 的 record node，避免缺 id 事件被错误归组。 */
        std::unordered_map<std::string, std::vector<size_t>> event_id_to_nodes;
        /**
         * @brief Raw Stream 到实际 DAG lane 的映射。
         *
         * Raw Stream 是 LD_PRELOAD / AscendCL wrapper 看到的 stream handle，需要通过 record event
         * 映射到实际 DAG lane。
         */
        std::unordered_map<std::string, std::string> raw_stream_to_stream;
        /**
         * @brief device lane 别名到真实 lane_key 的映射。
         *
         * streamId / Physic Stream Id / 顶层 tid 都可能是同一条 device lane 的别名；
         * stream sync 只能看到其中一种时，通过该表回到 DagBuilder::lane_key 选择出的真实 lane。
         */
        std::unordered_map<std::string, std::string> stream_alias_to_lane;

        /** @brief 特殊事件分类缓存，避免后续边构建阶段全图重复扫描。 */
        std::vector<size_t> event_record_nodes;
        std::vector<size_t> event_wait_nodes;
        std::vector<size_t> stream_sync_nodes;
        std::vector<size_t> event_sync_nodes;
        std::vector<size_t> device_sync_nodes;
        std::vector<size_t> notify_record_nodes;
        std::vector<size_t> notify_wait_nodes;
        std::vector<size_t> model_execute_nodes;
        std::vector<size_t> hccl_nodes;
    };

    /** @brief 归一化阶段合并重复事件、过滤 CPU 嵌套父节点，并保留 HiCache 事实事件。 */
    [[nodiscard]] static std::vector<TraceEvent> normalize_events(std::vector<TraceEvent> events);

    /** @brief 判断一个事件是否应被视作 device 执行节点；该判断会影响 lane 和边类型。 */
    [[nodiscard]] static bool is_device_event(const TraceEvent & event);

    /** @brief 判断事件是否属于 HiCache 事实事件，确保事实事件不会被 CPU leaf 过滤误删。 */
    [[nodiscard]] static bool is_hicache_event(const TraceEvent & event);

    /** @brief 计算顺序边使用的资源身份；该函数是 faithful replay 精度的关键点。 */
    [[nodiscard]] static std::string lane_key(const TraceEvent & event);

    /** @brief 从统一 args 表读取事件参数；只在缺失时返回 fallback。 */
    [[nodiscard]] static std::string event_arg(const TraceEvent & event, const std::string & key, const std::string & fallback = "");

    /**
     * @name 构图阶段
     *
     * 以下函数按固定顺序执行。顺序本身有语义：create_nodes 先收集 index，
     * correlation/sequence/sync 再根据这些 index 补边，最后统一收敛 sync 节点耗时。
     * @{
     */
    /** @brief 创建 DAG 节点并收集后续补边所需的事件索引。 */
    BuildIndex create_nodes(DagGraph & graph) const;

    /** @brief 根据 correlation_id 建立 CPU runtime 到 device kernel 的提交边。 */
    void add_correlation_edges(DagGraph & graph, BuildIndex & index) const;

    /** @brief 在每条 CPU/device lane 内建立顺序边。 */
    void add_sequential_edges(DagGraph & graph, BuildIndex & index) const;

    /** @brief 根据 EVENT_RECORD/EVENT_WAIT 建立 Ascend event wait 依赖。 */
    void add_event_wait_edges(DagGraph & graph, BuildIndex & index) const;

    /** @brief 为 NOTIFY_RECORD/NOTIFY_WAIT 这类模型执行同步锚点补边。 */
    void add_notify_wait_edges(DagGraph & graph, BuildIndex & index) const;

    /** @brief 为 MODEL_EXECUTE 到 device lane 建立保守同步边。 */
    void add_model_execute_edges(DagGraph & graph, BuildIndex & index) const;

    /** @brief 为 stream synchronize wrapper 建立 device-to-CPU wait 边。 */
    void add_stream_sync_edges(DagGraph & graph, BuildIndex & index) const;

    /** @brief 为 event synchronize wrapper 建立 record-to-sync wait 边。 */
    void add_event_sync_edges(DagGraph & graph, BuildIndex & index) const;

    /** @brief 为 device synchronize wrapper 建立所有 device lane 到 CPU 的 wait 边。 */
    void add_device_sync_edges(DagGraph & graph, BuildIndex & index) const;

    /** @brief 把同步 wait 节点耗时压缩为固定开销，避免重复计入观测阻塞。 */
    void finalize_sync_nodes(DagGraph & graph, const BuildIndex & index) const;
    /** @} */
};

} // namespace markov::trace_graph::core
