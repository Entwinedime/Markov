#include "trace_graph/modules/node_scale_module.hpp"

#include "trace_graph/core/trace_event.hpp"

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

namespace TraceGraph {

namespace {

bool read_u64(const std::unordered_map<std::string, std::string> & args, const std::string & key, uint64_t & value) {
    /**
     * @brief NodeScaleModule 读取的是节点 attrs，不读取原始 JSON。
     */
    const auto it = args.find(key);
    if (it == args.end()) return false;
    try {
        value = std::stoull(it->second);
        return true;
    }
    catch (...) {
        return false;
    }
}

const NodeScaleRuleConfig * find_matching_rule(const std::vector<NodeScaleRuleConfig> & rules, const std::string & name) {
    const auto match = std::ranges::find_if(rules, [&](const auto & rule) { return !rule.name.empty() && name.contains(rule.name); });
    return match == rules.end() ? nullptr : &*match;
}

} // namespace

NodeScaleModule::NodeScaleModule(NodeScaleConfig config) : config_(std::move(config)) {}

std::string NodeScaleModule::name() const { return "NodeScaleModule"; }

void NodeScaleModule::apply(DagGraph & graph) {
    applied_ = true;
    scaled_nodes_ = 0;
    /**
     * @brief 遍历 nodes() 的快照引用时不新增/删除节点，只改当前 node duration，因此安全。
     */
    for (const auto & node_snapshot : graph.nodes()) {
        auto node_id = node_snapshot.id;
        const auto & record = graph.event_for_node(node_id);
        /**
         * @brief 当前规则是简单 substring match，适合 smoke 和粗粒度 what-if。
         *
         * 精确匹配、正则或按 node kind 匹配应在配置 schema 中显式扩展。
         */
        const auto * rule = find_matching_rule(config_.rules, record.name);
        if (rule == nullptr) continue;

        uint64_t original_time = 0;
        if (!read_u64(graph.node(node_id).attrs, "ori_time", original_time)) continue;

        /**
         * @brief 旧 C++ 图对极短 CPU 节点保留固定开销。
         *
         * 子模块沿用该约定以保持 base DAG 行为。3000ns 以下节点不缩放，
         * 3000ns 以上只缩放超出固定开销的部分。
         */
        uint64_t new_time = original_time;
        if (original_time >= 3'000) new_time = static_cast<uint64_t>((original_time - 3'000) * rule->factor + 3'000);

        /**
         * @warning 如果节点带 cpuinterval，缩放后会把 interval 也加到节点耗时上。
         *
         * 这沿用旧逻辑，但与拓扑仿真中的 interval 计入方式存在交叉，需要审查是否重复计入。
         */
        uint64_t cpu_interval = 0;
        if (read_u64(graph.node(node_id).attrs, "cpuinterval", cpu_interval)) new_time += cpu_interval;
        graph.set_node_duration(node_id, new_time);
        ++scaled_nodes_;
    }
}

bool NodeScaleModule::has_summary() const { return applied_; }

std::string NodeScaleModule::summary_json() const {
    /**
     * @brief 手写 JSON 是为了保持模块不依赖额外输出结构；字符串字段必须显式 escape。
     */
    std::ostringstream os;
    os << "{\"name\":\"NodeScaleModule\",\"rule_count\":" << config_.rules.size() << ",\"scaled_nodes\":" << scaled_nodes_ << ",\"rules\":[";
    bool first = true;
    std::ranges::for_each(config_.rules, [&](const auto & rule) {
        if (!first) os << ",";
        first = false;
        os << "{\"id\":\"" << escape_json(rule.id) << "\",\"name\":\"" << escape_json(rule.name) << "\",\"factor\":" << rule.factor << "}";
    });
    os << "]}";
    return os.str();
}

} // namespace TraceGraph
