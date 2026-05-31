#include "trace_graph/frontend/trace_normalizer.hpp"

#include <string>

namespace TraceGraph {

namespace {

bool starts_with(const std::string & text, const std::string & prefix) { return text.rfind(prefix, 0) == 0; }

} // namespace

void normalize_trace_records(std::vector<std::unique_ptr<ActivityRecord>> & records) {
    for (auto & record : records) {
        if (!record) continue;

        if (record->cat == "hicache" || starts_with(record->name, "HiCache::")) {
            record->cat = "hicache";
            record->args.emplace("domain", "cache_io");
            record->args.emplace("producer", "python_probe");
            record->args.emplace("framework", "sglang");
            continue;
        }

        if (record->cat == "hook" || starts_with(record->name, "AscendCL@")) {
            record->args.emplace("producer", "native_hook");
            continue;
        }

        if (record->cat == "Kernel" || record->cat == "cpu_op") {
            record->args.emplace("producer", "torch_profiler");
        }
    }
}

} // namespace TraceGraph
