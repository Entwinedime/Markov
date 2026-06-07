#include "trace_graph/core/dag_graph.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace TraceGraph {

namespace {

// summary 中使用的边类型名称。保持小写英文，属于用户可见输出字段。
std::string edge_kind_name(DagEdgeKind kind) {
    switch (kind) {
    case DagEdgeKind::Sequential:
        return "sequential";
    case DagEdgeKind::Stream:
        return "stream";
    case DagEdgeKind::Correlation:
        return "correlation";
    case DagEdgeKind::Sync:
        return "sync";
    case DagEdgeKind::HCCL:
        return "hccl";
    case DagEdgeKind::HiCache:
        return "hicache";
    case DagEdgeKind::Mutation:
        return "mutation";
    }
    return "unknown";
}

bool contains_hccl_name(const std::string & name) {
    return name.find("hcom") != std::string::npos || name.find("HCCL") != std::string::npos || name.find("hccl") != std::string::npos;
}

// HCCL merge 时可能已经被子模块或前序逻辑改写 attrs["time"]。
// 优先使用 attrs["time"]，失败时回退到 node.duration。
uint64_t node_time(const DagNode & node) {
    auto it = node.attrs.find("time");
    if (it == node.attrs.end()) return node.duration;
    try {
        return std::stoull(it->second);
    }
    catch (...) {
        return node.duration;
    }
}

} // namespace

DagGraph::DagGraph(std::vector<TraceEvent> events, int gpu_id) : events_(std::move(events)), gpu_id_(gpu_id) {}

size_t DagGraph::add_node(size_t event_index, bool is_cpu, const std::string & lane_key) {
    if (event_index >= events_.size()) { throw std::out_of_range("event index out of range while adding DAG node"); }
    size_t node_id = nodes_.size();
    const auto & event = events_[event_index];
    DagNode node;
    node.id = node_id;
    node.event_index = event_index;
    node.gpu_id = gpu_id_;
    node.is_cpu = is_cpu;
    node.lane_key = lane_key;
    node.duration = event.dur;
    node.original_duration = event.dur;
    // attrs["time"] / attrs["ori_time"] 是老版 TraceGraph 与 NodeScaleModule 都会读取的字段。
    // duration 字段和 attrs["time"] 必须保持同步。
    node.attrs["time"] = std::to_string(event.dur);
    node.attrs["ori_time"] = std::to_string(event.dur);
    node.attrs["tid"] = lane_key;
    node.attrs["name"] = event.name;
    node.attrs["cat"] = event.cat;
    if (!is_cpu) node.attrs["gpuid"] = std::to_string(gpu_id_);
    nodes_.push_back(std::move(node));
    if (is_cpu) cpu_lanes_.insert(lane_key);
    return node_id;
}

void DagGraph::add_edge(size_t src, size_t dst, DagEdgeKind kind) {
    if (src >= nodes_.size() || dst >= nodes_.size()) { throw std::out_of_range("DAG edge endpoint is out of range"); }
    // 当前不做边去重。重复边不会改变 critical path，但会增加 indegree 和 summary edge_count。
    edges_.push_back(DagEdge{src, dst, kind});
}

const TraceEvent & DagGraph::event(size_t event_index) const {
    if (event_index >= events_.size()) { throw std::out_of_range("event index out of range"); }
    return events_[event_index];
}

TraceEvent & DagGraph::mutable_event(size_t event_index) {
    if (event_index >= events_.size()) { throw std::out_of_range("event index out of range"); }
    return events_[event_index];
}

const TraceEvent & DagGraph::event_for_node(size_t node_id) const { return event(node(node_id).event_index); }

TraceEvent & DagGraph::mutable_event_for_node(size_t node_id) { return mutable_event(node(node_id).event_index); }

const DagNode & DagGraph::node(size_t node_id) const {
    if (node_id >= nodes_.size()) { throw std::out_of_range("DAG node id out of range"); }
    return nodes_[node_id];
}

DagNode & DagGraph::mutable_node(size_t node_id) {
    if (node_id >= nodes_.size()) { throw std::out_of_range("DAG node id out of range"); }
    return nodes_[node_id];
}

void DagGraph::set_node_attr(size_t node_id, const std::string & key, const std::string & value) { mutable_node(node_id).attrs[key] = value; }

void DagGraph::set_node_duration(size_t node_id, uint64_t duration) {
    auto & node = mutable_node(node_id);
    node.duration = duration;
    node.attrs["time"] = std::to_string(duration);
}

std::string DagGraph::node_attr(size_t node_id, const std::string & key, const std::string & fallback) const {
    const auto & attrs = node(node_id).attrs;
    auto it = attrs.find(key);
    return it == attrs.end() ? fallback : it->second;
}

void DagGraph::set_cpu_lane(const std::string & lane_key) { cpu_lanes_.insert(lane_key); }

bool DagGraph::is_cpu_lane(const std::string & lane_key) const { return cpu_lanes_.find(lane_key) != cpu_lanes_.end(); }

std::unordered_map<std::string, size_t> DagGraph::edge_counts_by_kind() const {
    std::unordered_map<std::string, size_t> counts;
    for (const auto & edge : edges_) counts[edge_kind_name(edge.kind)]++;
    return counts;
}

DagGraph DagGraph::merge(std::vector<DagGraph> graphs) {
    if (graphs.empty()) return DagGraph();
    if (graphs.size() == 1) return std::move(graphs.front());

    // 先合并事件数组，再按 offset 平移每个子图的 node id / event_index / edge endpoint。
    // 这样每个 per-rank graph 内部的边保持不变，后面只追加跨 rank 边。
    std::vector<TraceEvent> merged_events;
    for (const auto & graph : graphs) {
        for (const auto & event : graph.events_) {
            TraceEvent copy = event;
            copy.index = merged_events.size();
            merged_events.push_back(std::move(copy));
        }
    }

    DagGraph merged(std::move(merged_events), 0);
    size_t event_offset = 0;
    size_t node_offset = 0;
    std::vector<size_t> node_offsets(graphs.size() + 1, 0);
    // ? HCCL group 的 key 当前只使用 kernel name，value 是 graph_index -> global node ids。
    // ? 这是老版兼容策略；如果能拿到 group/correlation，应验证是否需要收窄匹配条件。
    std::unordered_map<std::string, std::unordered_map<int, std::vector<size_t>>> hccl_groups;
    uint64_t real_min = 0;
    uint64_t real_max = 0;
    bool has_real_time = false;
    for (auto & graph : graphs) {
        size_t graph_index = (&graph - graphs.data());
        node_offsets[graph_index] = node_offset;
        merged.parsed_record_count_ += graph.parsed_record_count_;
        for (const auto & node : graph.nodes_) {
            DagNode copy = node;
            copy.id += node_offset;
            copy.event_index += event_offset;
            merged.nodes_.push_back(std::move(copy));
        }
        for (const auto & edge : graph.edges_) { merged.edges_.push_back(DagEdge{edge.src + node_offset, edge.dst + node_offset, edge.kind}); }
        for (const auto & lane : graph.cpu_lanes_) { merged.cpu_lanes_.insert(lane); }
        for (const auto & node : graph.nodes_) {
            auto it = node.attrs.find("name");
            if (it != node.attrs.end() && !node.is_cpu && contains_hccl_name(it->second)) {
                hccl_groups[it->second][static_cast<int>(graph_index)].push_back(node.id + node_offset);
            }
            const auto & event = graph.event_for_node(node.id);
            if (!has_real_time || event.ts < real_min) real_min = event.ts;
            auto event_end = event.ts + event.dur;
            if (event_end > real_max) real_max = event_end;
            has_real_time = true;
        }
        event_offset += graph.events_.size();
        node_offset += graph.nodes_.size();
    }
    node_offsets[graphs.size()] = node_offset;
    merged.real_e2e_time_ = has_real_time && real_max > real_min ? real_max - real_min : 0;

    // 老版 TraceGraph 在多卡 merge 时按 HCCL kernel 名称和序号对齐通信节点：
    // 其他 rank 的 HCCL 节点会约束本 rank HCCL 后继节点，并把同组通信耗时规约为最小值。
    //
    // 这里的关键假设：
    // - 每个 rank 上同名 HCCL kernel 的顺序一致；
    // - 同组通信的等待主要通过跨 rank 边表达；
    // - duration 取最小值可以剥离各 rank 上重复计入的等待。
    //
    // ? 如果 trace 中 repeated collective 名称相同但顺序不一致，这里可能错配。
    for (const auto & group_item : hccl_groups) {
        size_t max_count = 0;
        for (const auto & by_gpu : group_item.second) max_count = std::max(max_count, by_gpu.second.size());
        for (size_t comm_index = 0; comm_index < max_count; ++comm_index) {
            std::vector<std::pair<int, size_t>> current;
            for (const auto & by_gpu : group_item.second) {
                if (comm_index < by_gpu.second.size()) current.push_back({by_gpu.first, by_gpu.second[comm_index]});
            }

            uint64_t min_time = 0;
            for (const auto & item : current) {
                auto time = node_time(merged.nodes_[item.second]);
                if (min_time == 0 || time < min_time) min_time = time;
            }
            for (const auto & item : current) {
                // hccl_sync 是 per-rank 构图阶段记录的“本 rank HCCL 后继节点”。
                // 跨 rank 边从其他 rank 的 HCCL 指向该后继，表达本 rank 后续工作要等其他 rank 通信完成。
                auto next_it = merged.nodes_[item.second].attrs.find("hccl_sync");
                if (next_it == merged.nodes_[item.second].attrs.end()) continue;
                size_t next_local = 0;
                try {
                    next_local = std::stoull(next_it->second);
                }
                catch (...) {
                    continue;
                }
                if (item.first < 0 || static_cast<size_t>(item.first + 1) >= node_offsets.size()) continue;
                size_t next_global = next_local + node_offsets[static_cast<size_t>(item.first)];
                if (next_global >= merged.nodes_.size()) continue;
                for (const auto & other : current) {
                    if (other.first == item.first && other.second == item.second) continue;
                    merged.edges_.push_back(DagEdge{other.second, next_global, DagEdgeKind::HCCL});
                }
            }
            if (min_time > 0) {
                // 同组通信节点统一缩为最短 duration，避免把等待时间在所有 rank 上重复计入。
                for (const auto & item : current) merged.set_node_duration(item.second, min_time);
            }
        }
    }
    return merged;
}

} // namespace TraceGraph
