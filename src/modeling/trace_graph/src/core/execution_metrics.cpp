#include "core/execution_metrics.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/resource.h>
#include <utility>

namespace markov::trace_graph::core {

void ExecutionMetrics::checkpoint(std::string_view phase) {
    const auto now = Clock::now();
    const auto cpu = std::clock();
    phases_[std::string(phase)] = {
        { "wall_seconds",    std::chrono::duration<double>(now - previous_).count() },
        {  "cpu_seconds", static_cast<double>(cpu - cpu_previous_) / CLOCKS_PER_SEC }
    };
    previous_ = now;
    cpu_previous_ = cpu;
}

nlohmann::json ExecutionMetrics::report() const {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    nlohmann::json memory = {
        { "process_peak_rss_bytes",                                            static_cast<uint64_t>(usage.ru_maxrss) * 1'024 },
        {                  "scope", "one process; current PSS/private bytes are snapshots, not peaks; do not sum process RSS" }
    };

    const auto flag = std::getenv("TRACE_GRAPH_MEMORY_METRICS");
    const bool details = flag && std::string_view(flag) == "1";
    memory["detailed_snapshot_enabled"] = details;
    if (details) {
        // Linux rollup walks page tables; reserve that work for explicit diagnostics.
        std::ifstream input("/proc/self/smaps_rollup");
        std::string line;
        while (std::getline(input, line)) {
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            const auto name = line.substr(0, colon);
            if (name == "Rss" || name == "Pss" || name == "Private_Clean" || name == "Private_Dirty")
                memory[name + "_bytes"] = std::stoull(line.substr(colon + 1)) * 1'024;
        }
    }

    return {
        { "wall_seconds",    std::chrono::duration<double>(Clock::now() - started_).count() },
        {  "cpu_seconds", static_cast<double>(std::clock() - cpu_started_) / CLOCKS_PER_SEC },
        {       "phases",                                                           phases_ },
        {       "memory",                                                 std::move(memory) }
    };
}

} // namespace markov::trace_graph::core
