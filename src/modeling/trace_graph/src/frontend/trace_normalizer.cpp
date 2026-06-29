#include "markov/trace_graph/frontend/trace_normalizer.hpp"

#include <algorithm>
#include <ranges>
#include <string>

namespace markov::trace_graph::frontend {

void normalize_trace_events(std::vector<core::TraceEvent> & events) {
    /**
     * @brief normalizer 只补“来源事实”和统一类别，不做建模判断。
     *
     * 例如 hicache 事件只标记 domain/producer，是否命中、是否写回由后续 HiCacheModule 决定。
     */
    std::ranges::for_each(events, [](core::TraceEvent & event) {
        if (event.cat == "hicache" || event.name.starts_with("HiCache::") || event.name.starts_with("hicache_")) {
            event.cat = "hicache";
            event.args.emplace("domain", "hicache");
            event.args.emplace("producer", "python_probe");
            event.args.emplace("framework", "sglang");
            return;
        }

        if (event.cat == "hook" || event.cat == "ld_preload" || event.name.starts_with("AscendCL@")) {
            /**
             * @brief LD_PRELOAD 事件常用于补充 torch trace 缺失的 runtime 参数或 sync 边界。
             */
            event.args.emplace("producer", "ld_preload");
            return;
        }

        if (event.cat == "Kernel" || event.cat == "cpu_op" || event.cat == "runtime" || event.cat == "compute") {
            /**
             * @brief torch profiler 是 base DAG 的主要来源。
             */
            event.args.emplace("producer", "torch_profiler");
        }
    });
}

} // namespace markov::trace_graph::frontend
