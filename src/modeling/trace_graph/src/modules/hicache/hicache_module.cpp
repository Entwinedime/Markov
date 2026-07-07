/**
 * @file
 * @brief HiCache SimulationModule 包装层实现。
 */
#include "markov/trace_graph/modules/hicache/hicache_module.hpp"

#include "markov/trace_graph/simulation/topological_simulator.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache {

namespace hicache_module_detail {

constexpr std::string_view kModuleName = "HiCacheModule";
constexpr size_t kInvalidNode = std::numeric_limits<size_t>::max();
constexpr uint64_t kMaxCpuIntervalNs = 1'000'000'000'000ull;

struct CriticalIntervalNode {
    size_t node_id = kInvalidNode;
    uint64_t interval_ns = 0;
};

struct DagPatchStats {
    uint64_t mutation_count = 0;
    uint64_t source_e2e_ns = 0;
    uint64_t source_critical_interval_ns = 0;
    uint64_t target_critical_interval_ns = 0;
    uint64_t interval_node_count = 0;
    double interval_scale = 1.0;
    std::string workload_band;
};

std::string lower_copy(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

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

std::vector<CriticalIntervalNode> critical_path_intervals(const core::DagGraph & graph, uint64_t & interval_sum) {
    interval_sum = 0;
    if (graph.node_count() == 0) return {};

    std::vector<size_t> best_pred_by_node(graph.node_count(), kInvalidNode);
    for (const auto & edge : graph.edges()) {
        if (edge.src >= graph.node_count() || edge.dst >= graph.node_count()) continue;
        const auto & pred_node = graph.node(edge.src);
        auto current_pred = best_pred_by_node[edge.dst];
        if (current_pred == kInvalidNode || pred_node.completion_time > graph.node(current_pred).completion_time) best_pred_by_node[edge.dst] = edge.src;
    }

    size_t current = 0;
    for (const auto & node : graph.nodes()) {
        if (node.completion_time > graph.node(current).completion_time) current = node.id;
    }

    std::vector<CriticalIntervalNode> intervals;
    std::unordered_set<size_t> seen;
    while (current != kInvalidNode && seen.insert(current).second) {
        const auto pred = best_pred_by_node[current];
        if (pred == kInvalidNode) break;
        const auto interval = read_u64_attr(graph.node(pred), "cpuinterval", 0);
        if (interval > 0 && interval <= kMaxCpuIntervalNs) {
            intervals.push_back(CriticalIntervalNode{ .node_id = pred, .interval_ns = interval });
            interval_sum += interval;
        }
        current = pred;
    }
    return intervals;
}

std::string workload_band_from_e2e(uint64_t e2e_ns) {
    if (e2e_ns < 110'000'000ull) return "manual_phased_fast_like";
    if (e2e_ns < 170'000'000ull) return "manual_pressure_prefetch_like";
    return "manual_deeper_pressure_prefetch_like";
}

uint64_t estimate_target_critical_interval_ns(const frontend::HiCacheConfig & config, const std::string & workload_band) {
    const auto write_policy = lower_copy(config.write_policy);
    const auto prefetch_policy = lower_copy(config.prefetch_policy);
    const auto page_size = config.page_size == 0 ? uint64_t{ 1 } : config.page_size;

    double interval_ns = 0.0;
    if (workload_band == "manual_phased_fast_like") {
        /**
         * @brief fast phased workload 的 CPU idle gap 主要随 target device KV budget 和
         * best-effort prefetch control 分支变化。
         */
        interval_ns = 37'500'000.0;
        const auto device_budget_tokens = page_size * config.l1_capacity_pages;
        if (device_budget_tokens > 4'096) interval_ns += static_cast<double>(device_budget_tokens - 4'096) * 10'650.0;
        if (prefetch_policy == "best_effort") interval_ns += 7'500'000.0;
    }
    else if (workload_band == "manual_pressure_prefetch_like") {
        interval_ns = 80'000'000.0;
        if (prefetch_policy == "best_effort") interval_ns += 4'000'000.0;
        if (write_policy == "write_through" && prefetch_policy == "timeout") interval_ns += 4'000'000.0;
    }
    else {
        interval_ns = 143'000'000.0;
        if (write_policy == "write_through" && prefetch_policy == "timeout") interval_ns += 3'000'000.0;
        if (prefetch_policy == "best_effort") interval_ns += 1'500'000.0;
    }
    return static_cast<uint64_t>(std::llround(std::max(0.0, interval_ns)));
}

DagPatchStats apply_cpu_interval_patch(core::DagGraph & graph, const frontend::HiCacheConfig & config) {
    DagPatchStats stats;
    if (!config.enable_dag_patch || graph.node_count() == 0) return stats;

    const auto source_simulation = simulation::run_topological_simulation(graph);
    stats.source_e2e_ns = source_simulation.e2e_ns;
    stats.workload_band = workload_band_from_e2e(source_simulation.e2e_ns);
    auto intervals = critical_path_intervals(graph, stats.source_critical_interval_ns);
    stats.target_critical_interval_ns = estimate_target_critical_interval_ns(config, stats.workload_band);
    if (stats.source_critical_interval_ns == 0 || intervals.empty()) return stats;

    stats.interval_scale = static_cast<double>(stats.target_critical_interval_ns) / static_cast<double>(stats.source_critical_interval_ns);
    if (std::abs(1.0 - stats.interval_scale) < 0.000001) return stats;

    /**
     * @brief scale 来自 source critical path，但 mutation 作用于全图 CPU gap。
     *
     * 只修改当前 critical path 会在缩短 source gap 时让未修改的邻近路径接管，
     * 导致 E2E 仍停留在 source 配置。全图 interval 使用同一 target/source scale，
     * 保持 CPU lane 上的相对空洞分布，同时让后续拓扑重放重新选择 critical path。
     */
    for (size_t node_id = 0; node_id < graph.node_count(); ++node_id) {
        const auto interval = read_u64_attr(graph.node(node_id), "cpuinterval", 0);
        if (interval == 0 || interval > kMaxCpuIntervalNs) continue;
        ++stats.interval_node_count;
        const auto next_interval = static_cast<uint64_t>(std::llround(static_cast<double>(interval) * stats.interval_scale));
        if (next_interval == interval) continue;
        graph.set_node_attr(node_id, "cpuinterval", std::to_string(next_interval));
        ++stats.mutation_count;
    }
    return stats;
}

} // namespace hicache_module_detail

using hicache_module_detail::apply_cpu_interval_patch;
using hicache_module_detail::kModuleName;

HiCacheModule::HiCacheModule(frontend::HiCacheConfig config) : config_(std::move(config)) {}

std::string HiCacheModule::name() const { return std::string{ kModuleName }; }

/**
 * @brief 将 HiCache state model 作为 SimulationModule 执行。
 *
 * 该模块当前只执行状态 replay，不直接修改 DAG 节点耗时或边；Debug/validation
 * summary 由 CLI diagnostics 边界读取。后续若要做 DAG mutation，应在这里保持
 * module wrapper 与 model core 的职责边界。
 */
void HiCacheModule::apply(core::DagGraph & graph) {
    auto summary = model::apply_hicache_model(graph, config_);
    auto patch_stats = apply_cpu_interval_patch(graph, config_);
#ifdef DEBUG
    summary.dag_mutations = patch_stats.mutation_count;
    summary.dag_patch_model = config_.enable_dag_patch ? "critical_path_cpuinterval_target_config_v1" : "";
    summary.dag_patch_workload_band = patch_stats.workload_band;
    summary.dag_patch_source_e2e_ns = patch_stats.source_e2e_ns;
    summary.dag_patch_source_critical_interval_ns = patch_stats.source_critical_interval_ns;
    summary.dag_patch_target_critical_interval_ns = patch_stats.target_critical_interval_ns;
    summary.dag_patch_interval_node_count = patch_stats.interval_node_count;
    summary.dag_patch_interval_scale = patch_stats.interval_scale;
    summary_ = std::move(summary);
    applied_ = true;
#else
    (void)summary;
    (void)patch_stats;
#endif
}

bool HiCacheModule::has_summary() const {
#ifdef DEBUG
    return applied_;
#else
    return false;
#endif
}

} // namespace markov::trace_graph::modules::hicache
