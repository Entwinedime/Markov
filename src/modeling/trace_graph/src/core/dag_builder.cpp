/**
 * @file
 * @brief Chrome trace event 到 TraceGraph DAG 的构图实现。
 *
 * 构图阶段只从 trace 中提取可证明的执行顺序、提交链和同步边；what-if 模块和
 * HiCache state model 的策略判断必须在 DAG 建好后执行。
 */
#include "markov/trace_graph/core/dag_builder.hpp"

#include <algorithm>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

namespace markov::trace_graph::core {

namespace dag_builder_detail {

/**
 * @brief 用包含关系识别不同 trace 来源中的 HCCL 命名。
 *
 * 这个判断会影响跨 rank merge，因此宁可在后续收窄，也不要在这里加入业务无关关键字。
 */
bool contains_any_hccl_name(const std::string & name) { return name.contains("hcom") || name.contains("HCCL") || name.contains("hccl"); }

/**
 * @brief 读取 stream sync 查找前驱时使用的 launch timestamp。
 *
 * launchts 来自 correlation/connection 链的首个 CPU event，目的是让同步点按 launch 时刻而不是
 * kernel 完成时刻匹配目标 lane 上最近的已提交工作。
 */
uint64_t event_launch_ts(const DagGraph & graph, size_t node_id) {
    const auto & event = graph.event_for_node(node_id);
    auto launch = event.arg_u64("launchts", 0);
    return launch > 0 ? launch : event.ts;
}

/**
 * @brief 从 Ascend event 的 CPU wrapper 中读取可用 event id。
 *
 * wrapper 有时把 id 写成 "Event Id"，有时写成 event_id。缺 event id 时不能建
 * event record/wait 边，否则多个缺 id 事件会被错误归到一组。
 */
std::optional<std::string> event_id_from_cpu_record(const TraceEvent & event) {
    if (event.has_arg("Event Id")) {
        auto value = event.arg("Event Id");
        if (!value.empty()) return value;
    }
    if (event.has_arg("event_id")) {
        auto value = event.arg("event_id");
        if (!value.empty()) return value;
    }
    return std::nullopt;
}

uint64_t node_end_ts(const TraceEvent & event) { return event.ts + event.dur; }

bool is_usable_lane_value(const std::string & value) { return !value.empty() && value != "-1"; }

bool is_stream_sync_event(const std::string & name) {
    return name == "AscendCL@aclrtSynchronizeStream" || name == "AscendCL@aclrtSynchronizeStreamWithTimeout";
}

bool is_event_sync_event(const std::string & name) { return name == "AscendCL@aclrtSynchronizeEvent" || name == "AscendCL@aclrtSynchronizeEventWithTimeout"; }

bool is_device_sync_event(const std::string & name) {
    return name == "AscendCL@aclrtSynchronizeDevice" || name == "AscendCL@aclrtSynchronizeDeviceWithTimeout";
}

} // namespace dag_builder_detail

using dag_builder_detail::contains_any_hccl_name;
using dag_builder_detail::event_id_from_cpu_record;
using dag_builder_detail::event_launch_ts;
using dag_builder_detail::is_device_sync_event;
using dag_builder_detail::is_event_sync_event;
using dag_builder_detail::is_stream_sync_event;
using dag_builder_detail::is_usable_lane_value;
using dag_builder_detail::node_end_ts;

DagGraph DagBuilder::build(std::vector<TraceEvent> events, int gpu_id) const {
    auto parsed_count = events.size();
    auto normalized = normalize_events(std::move(events));
    DagGraph graph(std::move(normalized), gpu_id);
    graph.set_parsed_record_count(parsed_count);

    /**
     * @brief 构图顺序有依赖。
     *
     * 1. create_nodes 建立节点和索引；
     * 2. correlation 先写 launchts，后续 stream sync 会使用；
     * 3. sequential 写 cpuinterval 和 hccl_sync；
     * 4. sync 类边再消费上面生成的辅助字段；
     * 5. finalize 将 sync 节点耗时统一收敛为固定开销。
     */
    auto index = create_nodes(graph);
    add_correlation_edges(graph, index);
    add_sequential_edges(graph, index);
    add_event_wait_edges(graph, index);
    add_notify_wait_edges(graph, index);
    add_model_execute_edges(graph, index);
    add_stream_sync_edges(graph, index);
    add_event_sync_edges(graph, index);
    add_device_sync_edges(graph, index);
    finalize_sync_nodes(graph, index);

    /**
     * @brief real_e2e_time 是 trace 自身的真实时间窗口，仅用于 validation 对照。
     *
     * 拓扑仿真的 predicted E2E 会在 TopologicalSimulator 中计算。
     */
    uint64_t real_min = 0;
    uint64_t real_max = 0;
    bool has_real_time = false;
    for (const auto & node : graph.nodes()) {
        const auto & event = graph.event_for_node(node.id);
        if (!has_real_time || event.ts < real_min) real_min = event.ts;
        auto event_end = event.ts + event.dur;
        if (event_end > real_max) real_max = event_end;
        has_real_time = true;
    }
    graph.set_real_e2e_time(has_real_time && real_max > real_min ? real_max - real_min : 0);
    return graph;
}

std::vector<TraceEvent> DagBuilder::normalize_events(std::vector<TraceEvent> events) {
    std::unordered_map<std::string, std::vector<size_t>> groups_by_ts_dur;
    std::vector<bool> dropped(events.size(), false);

    /**
     * @brief 同时间同 duration 的 torch/LD_PRELOAD 合并事件表示同一个 runtime 边界。
     *
     * @warning 当前 key 只有 ts+dur，这是为大 trace 性能做的简化；如果不同事件碰巧完全同时间同耗时，
     * 这里可能误合并。是否需要加入 pid/tid/name/correlation 等字段收紧，需要用真实 trace 统计验证。
     */
    for (const auto i : std::views::iota(size_t{ 0 }, events.size())) {
        auto key = std::to_string(events[i].ts) + "_" + std::to_string(events[i].dur);
        groups_by_ts_dur[key].push_back(i);
    }
    for (auto & item : groups_by_ts_dur) {
        auto & indices = item.second;
        if (indices.size() <= 1) continue;
        /**
         * @brief 合并同一时间窗口内 NPU 事件和 wrapper 事件的参数。
         *
         * 如果同一时间窗口内存在带 Physic Stream Id 的 NPU 事件，就把其他 wrapper 事件的 args
         * 合并到这个 NPU 事件上，避免同一 runtime 边界被建成多个可执行节点。
         */
        size_t npu_index = static_cast<size_t>(-1);
        for (size_t index : indices) {
            if (events[index].has_arg("Physic Stream Id")) {
                npu_index = index;
                break;
            }
        }
        if (npu_index == static_cast<size_t>(-1)) continue;
        for (size_t index : indices) {
            if (index == npu_index) continue;
            for (const auto & kv : events[index].args) events[npu_index].args[kv.first] = kv.second;
            /**
             * @brief 老 trace 中 tid=0 的 CPU wrapper 名称通常更接近 runtime API 名称。
             *
             * 这里保留其 name，方便后续特殊事件识别。
             */
            if (events[index].tid == "0") events[npu_index].name = events[index].name;
            dropped[index] = true;
        }
    }

    std::vector<TraceEvent> deduped;
    deduped.reserve(events.size());
    for (const auto i : std::views::iota(size_t{ 0 }, events.size())) {
        if (dropped[i] && !is_hicache_event(events[i])) continue;
        deduped.push_back(std::move(events[i]));
    }
    std::ranges::sort(deduped, [](const TraceEvent & a, const TraceEvent & b) {
        if (a.ts != b.ts) return a.ts < b.ts;
        if (a.dur != b.dur) return a.dur > b.dur;
        return a.index < b.index;
    });

    /**
     * @brief 按 lane 建立 CPU 嵌套过滤所需的局部序列。
     *
     * device 事件不参与 CPU leaf 过滤，否则会把 kernel/copy 误删。
     */
    std::unordered_map<std::string, std::vector<size_t>> lane_to_indices;
    std::unordered_set<std::string> cpu_lanes;
    for (const auto i : std::views::iota(size_t{ 0 }, deduped.size())) {
        auto lane = lane_key(deduped[i]);
        lane_to_indices[lane].push_back(i);
        if (!is_device_event(deduped[i])) cpu_lanes.insert(lane);
    }

    std::vector<bool> is_leaf(deduped.size(), true);
    std::vector<bool> discarded(deduped.size(), false);
    std::vector<bool> is_enqueue_node(deduped.size(), false);
    std::unordered_set<std::string> seen_corr_ids;

    /**
     * @brief 只保留 CPU 嵌套调用树中的叶子节点。
     *
     * 这是老版 TraceGraph 的关键优化。torch profiler 中 CPU op 往往有父子嵌套，
     * 父子同时进入 DAG 会重复计算同一段 CPU 时间。这里用一个按 timestamp 排序的 stack
     * 近似判断嵌套关系。特例：
     * - Node@launch 或首个 correlation enqueue 节点代表 launch 边界，保留父节点并丢弃内部 Runtime；
     * - Runtime@ 开头的子节点常常只是 launch 内部实现细节，避免重复计时；
     * - AscendCL@aclrtRecordEvent 需要保留 record 语义，因此不把它当作普通父节点剔除。
     */
    for (auto & item : lane_to_indices) {
        if (!cpu_lanes.contains(item.first)) continue;
        auto & indices = item.second;
        std::ranges::sort(indices, [&](size_t a, size_t b) {
            if (deduped[a].ts != deduped[b].ts) return deduped[a].ts < deduped[b].ts;
            return a < b;
        });

        std::vector<size_t> stack;
        for (size_t curr_idx : indices) {
            auto & curr = deduped[curr_idx];
            if (curr.has_arg("correlation_id")) {
                auto cid = curr.arg("correlation_id");
                if (!seen_corr_ids.contains(cid)) {
                    is_enqueue_node[curr_idx] = true;
                    seen_corr_ids.insert(cid);
                }
            }

            while (!stack.empty()) {
                auto top_idx = stack.back();
                auto & top = deduped[top_idx];
                double curr_end_threshold = static_cast<double>(curr.ts) + static_cast<double>(curr.dur) * 0.5;
                double top_end = static_cast<double>(top.ts) + static_cast<double>(top.dur);

                /**
                 * @warning curr_end_threshold 使用 curr 半个 duration，是老版启发式。
                 *
                 * 当前事件如果明显落在 top 区间内，则 top 可能是 curr 的父区间；阈值是否合理
                 * 需要专门验证。
                 */
                if (top_end > curr_end_threshold) {
                    if (top.name == "Node@launch" || is_enqueue_node[top_idx] || curr.name.starts_with("Runtime@")) {
                        discarded[curr_idx] = true;
                        break;
                    }
                    if (top.name != "AscendCL@aclrtRecordEvent") is_leaf[top_idx] = false;
                    /**
                     * @brief 子事件缺 correlation_id 时继承父事件 id，让后续 launch->kernel 链仍能连起来。
                     */
                    if (top.has_arg("correlation_id") && !curr.has_arg("correlation_id")) curr.args["correlation_id"] = top.arg("correlation_id");
                    break;
                }
                stack.pop_back();
            }

            if (discarded[curr_idx]) continue;
            if (!stack.empty()) {
                /**
                 * @brief parent_seq 只用于 debug/导出，帮助审查某个 CPU leaf 是从哪条嵌套路径保留下来的。
                 */
                std::string parent_seq;
                for (size_t i = 0; i < stack.size(); ++i) {
                    if (i > 0) parent_seq += " -> ";
                    parent_seq += deduped[stack[i]].name + "(" + std::to_string(stack[i]) + ")";
                }
                curr.args["parent_seq"] = parent_seq;
            }
            stack.push_back(curr_idx);
        }
    }

    std::vector<TraceEvent> result;
    result.reserve(deduped.size());
    for (const auto i : std::views::iota(size_t{ 0 }, deduped.size())) {
        if ((is_leaf[i] && !discarded[i]) || is_hicache_event(deduped[i])) result.push_back(std::move(deduped[i]));
    }
    for (const auto i : std::views::iota(size_t{ 0 }, result.size())) result[i].index = i;
    return result;
}

bool DagBuilder::is_device_event(const TraceEvent & event) {
    /**
     * @brief Physic Stream Id 是 CANN/Ascend 侧最明确的 device 执行证据。
     */
    if (event.has_arg("Physic Stream Id")) return true;
    /**
     * @brief 部分 torch trace 没有 Physic Stream Id，但 Kernel/cpu_op 带 streamId 时也应进入 device lane。
     */
    if ((event.cat == "Kernel" || event.cat == "cpu_op") && event.has_arg("streamId")) return true;
    return false;
}

bool DagBuilder::is_hicache_event(const TraceEvent & event) {
    auto domain = event.arg("domain");
    return event.cat == "hicache" || event.name.starts_with("HiCache::") || event.name.starts_with("hicache_") || domain == "hicache"
           || (domain == "python_probe" && event.name.contains("hicache"));
}

std::string DagBuilder::lane_key(const TraceEvent & event) {
    if (!is_device_event(event)) return "CPU_MERGED";
    /**
     * @brief 对齐老版 TraceGraph 的 device lane 选择规则。
     *
     * device 事件虽然用 Physic Stream Id 判断是否在 NPU 上执行，但实际串行 lane 优先取
     * trace 顶层 tid，其次才取 streamId / Physic Stream Id。reader 会把顶层 tid 也写入
     * args["tid"]，这里必须直接读 event.tid，避免 args fallback 抢先命中。
     */
    if (is_usable_lane_value(event.tid)) return event.tid;
    auto stream_id = event.arg("streamId", event.arg("stream id"));
    if (is_usable_lane_value(stream_id)) return stream_id;
    auto physic_stream_id = event.arg("Physic Stream Id");
    if (is_usable_lane_value(physic_stream_id)) return physic_stream_id;
    return "NPU_UNKNOWN";
}

std::string DagBuilder::event_arg(const TraceEvent & event, const std::string & key, const std::string & fallback) { return event.arg(key, fallback); }

DagBuilder::BuildIndex DagBuilder::create_nodes(DagGraph & graph) const {
    BuildIndex index;
    std::string cpu_merged_pid = "-1";
    std::string cpu_merged_tid = "-1";

    for (const auto event_index : std::views::iota(size_t{ 0 }, graph.events().size())) {
        auto & event = graph.mutable_event(event_index);
        bool is_device = is_device_event(event);
        auto lane = lane_key(event);
        auto node_id = graph.add_node(event_index, !is_device, lane);
        auto & node = graph.mutable_node(node_id);
        index.lane_to_nodes[lane].push_back(node_id);

        if (!is_device) {
            /**
             * @warning 当前 faithful replay 将所有 CPU event 合并到一个 CPU lane。
             *
             * 这会保守地串行化多线程 CPU 工作；是否过度高估 CPU critical path 需要真实 trace 验证。
             */
            graph.set_cpu_lane(lane);
            if (cpu_merged_pid == "-1") {
                cpu_merged_pid = event.pid;
                cpu_merged_tid = event.tid;
            }
            node.attrs["sim_pid"] = cpu_merged_pid;
            node.attrs["sim_tid"] = cpu_merged_tid;
        }
        else {
            node.attrs["gpuid"] = std::to_string(graph.gpu_id());
            /**
             * @brief 登记 device lane 的可确认别名。
             *
             * 同一个 device lane 在 trace 中可能被顶层 tid、streamId、stream id、Physic Stream Id
             * 或 LD_PRELOAD 的 Raw Stream 描述。这里登记可确认别名，供 sync wrapper 反查真实 lane。
             */
            if (is_usable_lane_value(event.tid)) index.stream_alias_to_lane[event.tid] = lane;
            auto stream_id = event.arg("streamId");
            if (is_usable_lane_value(stream_id)) index.stream_alias_to_lane[stream_id] = lane;
            auto stream_id_alt = event.arg("stream id");
            if (is_usable_lane_value(stream_id_alt)) index.stream_alias_to_lane[stream_id_alt] = lane;
            auto physic_stream_id = event.arg("Physic Stream Id");
            if (is_usable_lane_value(physic_stream_id)) index.stream_alias_to_lane[physic_stream_id] = lane;
        }
        if (event.has_arg("parent_seq")) node.attrs["parent_seq"] = event.arg("parent_seq");

        /**
         * @brief connection_id/correlation_id 后续都用于建立 CPU runtime 与 device event 的因果链。
         */
        if (event.has_arg("connection_id")) index.connection_to_nodes[event.arg("connection_id")].push_back(node_id);
        if (event.has_arg("correlation_id")) index.correlation_to_nodes[event.arg("correlation_id")].push_back(node_id);
        if (contains_any_hccl_name(event.name) && is_device) index.hccl_nodes.push_back(node_id);

        /**
         * @brief 特殊事件只在这里分类一次。
         *
         * LD_PRELOAD wrapper 名称必须在这里显式识别，否则它只会作为普通 CPU 节点进入 DAG，
         * 不会产生 sync 语义边。
         */
        if (event.name == "EVENT_RECORD") index.event_record_nodes.push_back(node_id);
        else if (event.name == "EVENT_WAIT") {
            /**
             * @brief 把 EVENT_WAIT 向后挪 1ns，并把正 duration 减 1ns。
             *
             * 这样 record/wait 在相同 timestamp 附近时不会形成自相矛盾边界。
             */
            event.ts += 1;
            if (event.dur > 0) event.dur -= 1;
            index.event_wait_nodes.push_back(node_id);
        }
        else if (is_stream_sync_event(event.name)) index.stream_sync_nodes.push_back(node_id);
        else if (is_event_sync_event(event.name)) index.event_sync_nodes.push_back(node_id);
        else if (is_device_sync_event(event.name)) index.device_sync_nodes.push_back(node_id);
        else if (event.name == "NOTIFY_RECORD") index.notify_record_nodes.push_back(node_id);
        else if (event.name == "MODEL_EXECUTE") index.model_execute_nodes.push_back(node_id);
    }
    return index;
}

void DagBuilder::add_correlation_edges(DagGraph & graph, BuildIndex & index) const {
    /**
     * @brief 根据 correlation_id 建立 CPU runtime 与 device kernel 的提交链。
     *
     * correlation_id 是 torch profiler 中 CPU runtime 与 device kernel 的常见关联字段。
     * 同一个 id 下按时间排序后串起来，表示 launch/runtime/kernel 的提交链。
     */
    for (auto & item : index.correlation_to_nodes) {
        auto & nodes = item.second;
        if (nodes.size() <= 1) continue;
        std::ranges::sort(nodes, [&](size_t a, size_t b) {
            if (graph.event_for_node(a).ts != graph.event_for_node(b).ts) return graph.event_for_node(a).ts < graph.event_for_node(b).ts;
            return a < b;
        });
        for (size_t i = 1; i < nodes.size(); ++i) {
            /**
             * @brief launchts 记录这条链的第一个事件时间，用于后续 stream sync 匹配。
             */
            graph.mutable_event_for_node(nodes[i]).args["launchts"] = std::to_string(graph.event_for_node(nodes.front()).ts);
            graph.add_edge(nodes[i - 1], nodes[i], DagEdgeKind::Correlation);
        }
    }

    /**
     * @brief 根据 connection_id 建立 CANN runtime 与 device event 的连接链。
     *
     * 如果链长度 >=3 且首个事件不是 Node@launch，容易把不相关 runtime 误串起来，
     * 因此这类链不作为可靠的提交证据。
     */
    for (auto & item : index.connection_to_nodes) {
        auto & nodes = item.second;
        if (nodes.size() <= 1) continue;
        std::ranges::sort(nodes, [&](size_t a, size_t b) {
            if (graph.event_for_node(a).ts != graph.event_for_node(b).ts) return graph.event_for_node(a).ts < graph.event_for_node(b).ts;
            return a < b;
        });
        if (nodes.size() >= 3 && graph.event_for_node(nodes.front()).name != "Node@launch") continue;
        auto launch_ts = graph.event_for_node(nodes.front()).ts;
        for (size_t node_id : nodes) graph.mutable_event_for_node(node_id).args["launchts"] = std::to_string(launch_ts);
        for (size_t i = 1; i < nodes.size(); ++i) graph.add_edge(nodes[i - 1], nodes[i], DagEdgeKind::Correlation);
    }
}

void DagBuilder::add_sequential_edges(DagGraph & graph, BuildIndex & index) const {
    /**
     * @brief 每个 lane 内按 timestamp 串行。
     *
     * CPU lane 产生 Sequential 边，device lane 产生 Stream 边；拓扑仿真阶段二者都是 hard dependency，
     * 但边类型可以帮助 summary/debug 判断误差来源。
     */
    for (auto & item : index.lane_to_nodes) {
        auto & nodes = item.second;
        bool is_cpu = graph.is_cpu_lane(item.first);
        std::ranges::sort(nodes, [&](size_t a, size_t b) {
            if (graph.event_for_node(a).ts != graph.event_for_node(b).ts) return graph.event_for_node(a).ts < graph.event_for_node(b).ts;
            return a < b;
        });
        if (nodes.empty()) continue;

        for (size_t i = 0; i < nodes.size(); ++i) {
            auto node_id = nodes[i];
            auto & node = graph.mutable_node(node_id);
            const auto & event = graph.event_for_node(node_id);
            node.attrs["tid"] = item.first;
            node.attrs["name"] = event.name;
            node.attrs["cat"] = event.cat;
            node.attrs["time"] = std::to_string(event.dur);
            node.attrs["ori_time"] = node.attrs["time"];
            node.duration = event.dur;
            node.original_duration = event.dur;

            if (i == 0) continue;
            auto prev_node_id = nodes[i - 1];
            const auto & prev = graph.event_for_node(prev_node_id);
            graph.add_edge(prev_node_id, node_id, is_cpu ? DagEdgeKind::Sequential : DagEdgeKind::Stream);
            if (is_cpu) {
                /**
                 * @brief cpuinterval 是两个 CPU leaf 之间的空洞。
                 *
                 * 仿真会把它计入关键路径上的前驱后面，用来表达 CPU lane 空闲但实际 wall time
                 * 仍在推进的情况。
                 */
                auto interval = event.ts > prev.ts + prev.dur ? event.ts - (prev.ts + prev.dur) : 0;
                graph.set_node_attr(prev_node_id, "cpuinterval", std::to_string(interval));
            }
            /**
             * @brief NOTIFY_WAIT 只有当前一个节点是 MODEL_EXECUTE 时才进入 notify wait 集合。
             */
            if (event.name == "NOTIFY_WAIT" && prev.name == "MODEL_EXECUTE") index.notify_wait_nodes.push_back(node_id);
            /**
             * @brief HCCL 后继节点记录为本 rank 上通信完成后的第一个同 lane 节点。
             *
             * 多 rank merge 时其他 rank 的 HCCL 会连接到这个后继节点。
             */
            if (contains_any_hccl_name(prev.name) && !is_cpu) graph.set_node_attr(prev_node_id, "hccl_sync", std::to_string(node_id));
        }
    }
}

void DagBuilder::add_event_wait_edges(DagGraph & graph, BuildIndex & index) const {
    /**
     * @brief 第一阶段：从 CPU record wrapper 找到 Event Id 和 Raw Stream。
     *
     * 这些字段随后绑定到对应 EVENT_RECORD device node，用于 event wait 和 stream sync 建边。
     */
    for (size_t record_node : index.event_record_nodes) {
        const auto & record_event = graph.event_for_node(record_node);
        auto conn_it = index.connection_to_nodes.find(record_event.arg("connection_id"));
        if (conn_it == index.connection_to_nodes.end() || conn_it->second.empty()) continue;
        auto cpu_node = conn_it->second.front();
        const auto & cpu_event = graph.event_for_node(cpu_node);
        /**
         * @brief Raw Stream 是后续 aclrtSynchronizeStream 可能唯一能提供的 stream 证据。
         */
        if (cpu_event.has_arg("Raw Stream")) {
            auto lane = graph.node(record_node).lane_key;
            index.raw_stream_to_stream[cpu_event.arg("Raw Stream")] = lane;
            index.stream_alias_to_lane[cpu_event.arg("Raw Stream")] = lane;
        }
        auto event_id = event_id_from_cpu_record(cpu_event);
        if (event_id) index.event_id_to_nodes[*event_id].push_back(record_node);
    }

    for (auto & item : index.event_id_to_nodes) {
        std::ranges::sort(item.second, [&](size_t a, size_t b) { return graph.event_for_node(a).ts < graph.event_for_node(b).ts; });
    }

    /**
     * @brief 第二阶段：为每个 EVENT_WAIT 连接同 event id 的最近 EVENT_RECORD。
     *
     * 如果 record 和 wait 不在同一 lane，则建立跨 lane sync 边。
     */
    for (size_t wait_node : index.event_wait_nodes) {
        const auto & wait_event = graph.event_for_node(wait_node);
        auto conn_it = index.connection_to_nodes.find(wait_event.arg("connection_id"));
        if (conn_it == index.connection_to_nodes.end() || conn_it->second.empty()) continue;
        auto cpu_node = conn_it->second.front();
        auto eid = event_id_from_cpu_record(graph.event_for_node(cpu_node));
        if (!eid) continue;
        auto events_it = index.event_id_to_nodes.find(*eid);
        if (events_it == index.event_id_to_nodes.end()) continue;

        auto & records = events_it->second;
        auto bound = records.end();
        if (wait_event.dur == 0) {
            /**
             * @brief 零 duration 的 EVENT_WAIT 不能匹配同 timestamp 的 EVENT_RECORD。
             *
             * 否则大时间戳 double 精度会把 ts - 0.1 四舍五入回 ts，多生成 sync 边。
             */
            auto bound_value = wait_event.ts > 0 ? wait_event.ts - 1 : 0;
            bound = std::upper_bound(records.begin(), records.end(), bound_value, [&](uint64_t value, size_t node_id) {
                return value < graph.event_for_node(node_id).ts;
            });
        }
        else {
            auto bound_value = static_cast<double>(wait_event.ts) + static_cast<double>(wait_event.dur) - 0.1;
            bound = std::upper_bound(records.begin(), records.end(), bound_value, [&](double value, size_t node_id) {
                return value < static_cast<double>(graph.event_for_node(node_id).ts);
            });
        }
        if (bound == records.begin()) continue;
        --bound;
        /**
         * @brief 如果最近 record 与 wait 在同 lane，它已经被 stream 顺序边覆盖。
         *
         * 继续向前找跨 lane record，避免生成没有新增约束价值的同 lane sync 边。
         */
        while (bound != records.begin() && graph.node(wait_node).lane_key == graph.node(*bound).lane_key) --bound;
        if (graph.node(wait_node).lane_key == graph.node(*bound).lane_key) continue;
        graph.add_edge(*bound, wait_node, DagEdgeKind::Sync);
    }
}

void DagBuilder::add_notify_wait_edges(DagGraph & graph, BuildIndex & index) const {
    /**
     * @brief 为 NOTIFY_RECORD/NOTIFY_WAIT 模型执行同步锚点建边。
     *
     * 当前策略是为每个 wait 找到 wait_end 之前 200ns 窗口内最近的 record。
     */
    std::ranges::sort(index.notify_record_nodes, [&](size_t a, size_t b) { return graph.event_for_node(a).ts < graph.event_for_node(b).ts; });
    for (size_t wait_node : index.notify_wait_nodes) {
        auto wait_end = node_end_ts(graph.event_for_node(wait_node));
        auto bound_value = wait_end > 200 ? wait_end - 200 : 0;
        auto it = std::upper_bound(index.notify_record_nodes.begin(), index.notify_record_nodes.end(), bound_value, [&](uint64_t value, size_t node_id) {
            return value < graph.event_for_node(node_id).ts;
        });
        if (it == index.notify_record_nodes.begin()) continue;
        --it;
        graph.add_edge(*it, wait_node, DagEdgeKind::Sync);
    }
}

void DagBuilder::add_model_execute_edges(DagGraph & graph, BuildIndex & index) const {
    /**
     * @brief 建立 MODEL_EXECUTE 到 device lane 的保守同步边。
     *
     * 在 model_start 到最早 notify_wait_end 之间，每条 device lane 的首个节点依赖
     * MODEL_EXECUTE。这样能避免模型执行窗口内的 device 工作被拓扑仿真提前到 CPU 调度之前。
     */
    for (size_t model_node : index.model_execute_nodes) {
        const auto & model_event = graph.event_for_node(model_node);
        auto model_start = model_event.ts;
        uint64_t notify_end = model_start;
        for (size_t wait_node : index.notify_wait_nodes) {
            const auto & wait_event = graph.event_for_node(wait_node);
            if (wait_event.ts >= model_start) {
                auto end = node_end_ts(wait_event);
                if (notify_end == model_start || end < notify_end) notify_end = end;
            }
        }
        if (notify_end <= model_start) continue;

        for (const auto & item : index.lane_to_nodes) {
            if (graph.is_cpu_lane(item.first) || item.first == graph.node(model_node).lane_key) continue;
            for (size_t node_id : item.second) {
                const auto & event = graph.event_for_node(node_id);
                if (event.ts >= model_start && event.ts <= notify_end) {
                    graph.add_edge(model_node, node_id, DagEdgeKind::Sync);
                    break;
                }
            }
        }
    }
}

void DagBuilder::add_stream_sync_edges(DagGraph & graph, BuildIndex & index) const {
    /**
     * @brief 为 aclrtSynchronizeStream 建立 device-to-CPU sync 边。
     *
     * aclrtSynchronizeStream 表示 CPU 等待某个 stream 上已经提交的工作完成。因此需要把目标 lane 上
     * sync 开始前最近的 device 节点连到这个 CPU sync 节点。
     */
    for (size_t sync_node : index.stream_sync_nodes) {
        auto & sync_event = graph.mutable_event_for_node(sync_node);
        std::vector<std::string> target_lanes;
        std::unordered_set<std::string> seen_lanes;
        auto add_lane_candidate = [&](const std::string & sid) {
            if (!is_usable_lane_value(sid)) return;
            std::string lane;
            auto raw_it = index.raw_stream_to_stream.find(sid);
            if (raw_it != index.raw_stream_to_stream.end()) lane = raw_it->second;
            auto alias_it = index.stream_alias_to_lane.find(sid);
            if (lane.empty() && alias_it != index.stream_alias_to_lane.end()) lane = alias_it->second;
            if (lane.empty() && index.lane_to_nodes.contains(sid)) lane = sid;
            if (!lane.empty() && seen_lanes.insert(lane).second) target_lanes.push_back(lane);
        };

        bool has_stream_evidence = false;
        auto consume_evidence = [&](const std::string & sid) {
            if (!is_usable_lane_value(sid)) return;
            has_stream_evidence = true;
            add_lane_candidate(sid);
        };
        consume_evidence(sync_event.arg("Raw Stream"));
        consume_evidence(sync_event.arg("streamId"));
        consume_evidence(sync_event.arg("stream id"));
        consume_evidence(sync_event.arg("Physic Stream Id"));

        if (!has_stream_evidence) {
            /**
             * @brief 缺少 stream 证据时，保守地等待所有 device lane。
             */
            for (const auto & item : index.lane_to_nodes) {
                if (!graph.is_cpu_lane(item.first)) target_lanes.push_back(item.first);
            }
        }

        for (const auto & lane : target_lanes) {
            auto & nodes = index.lane_to_nodes[lane];
            /**
             * @brief lower_bound 查找 launch time 早于 sync 开始的最后一个节点。
             *
             * 使用 launchts 而不是 event.ts，是为了把 CPU launch 与 device kernel 的提交关系考虑进去。
             */
            auto bound = std::lower_bound(nodes.begin(), nodes.end(), sync_event.ts, [&](size_t node_id, uint64_t value) {
                return event_launch_ts(graph, node_id) < value;
            });
            if (bound == nodes.begin()) continue;
            --bound;
            graph.add_edge(*bound, sync_node, DagEdgeKind::Sync);
        }
    }
}

void DagBuilder::add_event_sync_edges(DagGraph & graph, BuildIndex & index) const {
    /**
     * @brief 为 aclrtSynchronizeEvent 建立 event record 到 CPU sync 的依赖边。
     *
     * aclrtSynchronizeEvent 不一定伴随底层 EVENT_WAIT 节点，因此直接用 Event Id 连接最近的 EVENT_RECORD。
     */
    for (size_t sync_node : index.event_sync_nodes) {
        const auto & sync_event = graph.event_for_node(sync_node);
        auto event_id = event_id_from_cpu_record(sync_event);
        if (!event_id && sync_event.has_arg("connection_id")) {
            auto conn_it = index.connection_to_nodes.find(sync_event.arg("connection_id"));
            if (conn_it != index.connection_to_nodes.end() && !conn_it->second.empty())
                event_id = event_id_from_cpu_record(graph.event_for_node(conn_it->second.front()));
        }
        if (!event_id) continue;
        auto records_it = index.event_id_to_nodes.find(*event_id);
        if (records_it == index.event_id_to_nodes.end()) continue;

        auto & records = records_it->second;
        auto bound_value = node_end_ts(sync_event);
        auto bound = std::upper_bound(records.begin(), records.end(), bound_value, [&](uint64_t value, size_t node_id) {
            return value < graph.event_for_node(node_id).ts;
        });
        if (bound == records.begin()) continue;
        --bound;
        graph.add_edge(*bound, sync_node, DagEdgeKind::Sync);
    }
}

void DagBuilder::add_device_sync_edges(DagGraph & graph, BuildIndex & index) const {
    /**
     * @brief 为 aclrtSynchronizeDevice 建立所有 device lane 到 CPU sync 的依赖边。
     *
     * aclrtSynchronizeDevice 等价于 CPU 等待当前 device 上所有已提交 stream。在单 rank graph 内
     * 保守连接每条 device lane 上 sync 开始前最近的节点。
     */
    for (size_t sync_node : index.device_sync_nodes) {
        const auto & sync_event = graph.event_for_node(sync_node);
        for (const auto & item : index.lane_to_nodes) {
            if (graph.is_cpu_lane(item.first)) continue;
            auto & nodes = index.lane_to_nodes[item.first];
            auto bound = std::lower_bound(nodes.begin(), nodes.end(), sync_event.ts, [&](size_t node_id, uint64_t value) {
                return event_launch_ts(graph, node_id) < value;
            });
            if (bound == nodes.begin()) continue;
            --bound;
            graph.add_edge(*bound, sync_node, DagEdgeKind::Sync);
        }
    }
}

void DagBuilder::finalize_sync_nodes(DagGraph & graph, const BuildIndex & index) const {
    /**
     * @brief 将同步 wait 类节点压成固定开销。
     *
     * 老版 TraceGraph 将同步 wait 类节点压成固定 10ns。真正的等待时间由前驱边推动
     * critical path 体现，避免把观测到的阻塞 dur 重复计入。
     */
    auto set_fixed_sync_duration = [&](const std::vector<size_t> & nodes) {
        std::ranges::for_each(nodes, [&](size_t node_id) { graph.set_node_duration(node_id, 10); });
    };
    set_fixed_sync_duration(index.stream_sync_nodes);
    set_fixed_sync_duration(index.event_sync_nodes);
    set_fixed_sync_duration(index.device_sync_nodes);
    set_fixed_sync_duration(index.event_wait_nodes);
    set_fixed_sync_duration(index.notify_wait_nodes);
}

} // namespace markov::trace_graph::core
