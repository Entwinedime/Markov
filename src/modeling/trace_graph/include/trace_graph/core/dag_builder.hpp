#pragma once

#include "trace_graph/core/dag_graph.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace TraceGraph {

// DagBuilder 把一组 TraceEvent 转换成可仿真的 base DAG。
//
// 它只负责 faithful replay 的基础因果边：
// - 同 CPU lane 的顺序边；
// - 同 device stream/lane 的顺序边；
// - runtime launch 到 kernel 的 correlation 边；
// - event/stream sync 边；
// - 少量老版 TraceGraph 兼容的 notify/model_execute 边。
//
// what-if 子模块不应该把额外策略逻辑塞进 DagBuilder，而应该在 SimulationModule 中修改 DAG。
class DagBuilder {
public:
    DagGraph build(std::vector<TraceEvent> events, int gpu_id) const;

private:
    // 构图过程中反复需要按不同 key 查节点。BuildIndex 只在一次 build 内有效。
    struct BuildIndex {
        // lane -> node ids，用于同 lane 顺序边和 stream sync 查找。
        std::unordered_map<std::string, std::vector<size_t>> lane_to_nodes;
        // correlation_id / connection_id 是 runtime 与 device event 的主要关联证据。
        std::unordered_map<std::string, std::vector<size_t>> correlation_to_nodes;
        std::unordered_map<std::string, std::vector<size_t>> connection_to_nodes;
        // event_id_to_nodes 只应该收集可确认 event id 的 record node。
        std::unordered_map<std::string, std::vector<size_t>> event_id_to_nodes;
        // Raw Stream 是 LD_PRELOAD / AscendCL wrapper 看到的 stream handle；
        // 它需要通过 record event 映射到实际 DAG lane。
        std::unordered_map<std::string, std::string> raw_stream_to_stream;
        // streamId / Physic Stream Id / 顶层 tid 都可能是同一条 device lane 的别名。
        // stream sync 只能看到其中一种时，通过该表回到 DagBuilder::lane_key 选择出的真实 lane。
        std::unordered_map<std::string, std::string> stream_alias_to_lane;

        // 特殊事件分类缓存，避免后续边构建阶段全图重复扫描。
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

    // 归一化阶段会合并重复事件、过滤 CPU 嵌套父节点，并保留 HiCache 事实事件。
    static std::vector<TraceEvent> normalize_events(std::vector<TraceEvent> events);
    // 判断一个事件是否应该被视作 device 执行节点。这个判断会影响 lane 和边类型。
    static bool is_device_event(const TraceEvent & event);
    static bool is_hicache_event(const TraceEvent & event);
    // lane_key 是顺序边的资源身份。这个函数是 faithful replay 精度的关键点。
    static std::string lane_key(const TraceEvent & event);
    static std::string event_arg(const TraceEvent & event, const std::string & key, const std::string & fallback = "");

    // 以下函数按固定顺序执行。顺序本身有语义：create_nodes 先收集 index，
    // correlation/sequence/sync 再根据这些 index 补边，最后统一收敛 sync 节点耗时。
    BuildIndex create_nodes(DagGraph & graph) const;
    void add_correlation_edges(DagGraph & graph, BuildIndex & index) const;
    void add_sequential_edges(DagGraph & graph, BuildIndex & index) const;
    void add_event_wait_edges(DagGraph & graph, BuildIndex & index) const;
    void add_notify_wait_edges(DagGraph & graph, BuildIndex & index) const;
    void add_model_execute_edges(DagGraph & graph, BuildIndex & index) const;
    void add_stream_sync_edges(DagGraph & graph, BuildIndex & index) const;
    void add_event_sync_edges(DagGraph & graph, BuildIndex & index) const;
    void add_device_sync_edges(DagGraph & graph, BuildIndex & index) const;
    void finalize_sync_nodes(DagGraph & graph, const BuildIndex & index) const;
};

} // namespace TraceGraph
