#include "trace_graph/frontend/trace_normalizer.hpp"

#include <string>

namespace TraceGraph {

namespace {

bool starts_with(const std::string & text, const std::string & prefix) { return text.rfind(prefix, 0) == 0; }

} // namespace

void normalize_trace_events(std::vector<TraceEvent> & events) {
    /**
     * @brief normalizer 只补“来源事实”和统一类别，不做建模判断。
     *
     * 例如 hicache 事件只标记 domain/producer，是否命中、是否写回由后续 HiCacheModule 决定。
     */
    for (auto & event : events) {
        if (event.cat == "hicache" || starts_with(event.name, "HiCache::") || starts_with(event.name, "hicache_")) {
            event.cat = "hicache";
            event.args.emplace("domain", "hicache");
            event.args.emplace("producer", "python_probe");
            event.args.emplace("framework", "sglang");
            continue;
        }

        if (event.cat == "hook" || event.cat == "ld_preload" || starts_with(event.name, "AscendCL@")) {
            /**
             * @brief LD_PRELOAD 事件常用于补充 torch trace 缺失的 runtime 参数或 sync 边界。
             */
            event.args.emplace("producer", "ld_preload");
            continue;
        }

        if (event.cat == "Kernel" || event.cat == "cpu_op" || event.cat == "runtime" || event.cat == "compute") {
            /**
             * @brief torch profiler 是 base DAG 的主要来源。
             */
            event.args.emplace("producer", "torch_profiler");
        }
    }
}

} // namespace TraceGraph
