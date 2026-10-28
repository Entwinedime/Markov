#pragma once

#include "markov/adapters/costs.hpp"
#include "markov/adapters/sglang/cache.hpp"
#include "markov/trace_graph/planning/native_calls.hpp"
#include "markov/trace_graph/source/context.hpp"
#include "markov/trace_graph/source/manifest.hpp"

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace markov::adapters {

// One prepared capture owns its provenance and immutable source context.
// Reports and target planners borrow it; neither reopens the trace files.
struct PreparedSource {
    trace_graph::source::ProfileManifest manifest;
    trace_graph::source::SourceContext context;
    std::vector<std::size_t> input_record_counts;
    std::map<std::string, std::size_t> native_roles;
    double read_and_interpret_sec;
    double build_sec;
    std::optional<SourceCpuCosts> cpu_costs;
};

struct PageWorkRequest {
    trace_graph::source::ProfileManifest manifest;
    std::vector<std::size_t> page_sizes_tokens;
    std::optional<std::filesystem::path> models_path = std::nullopt;
    std::optional<sglang::CacheSettings> cache = std::nullopt;
    std::optional<std::filesystem::path> cpu_costs_path = std::nullopt;
    std::optional<trace_graph::planning::KernelCacheInitial> kernel_cache_initial = std::nullopt;
};

PreparedSource prepare_source(trace_graph::source::ProfileManifest manifest, const std::optional<std::filesystem::path> & cpu_costs_path = std::nullopt);
PageWorkRequest read_page_work_request(const std::filesystem::path & path);

} // namespace markov::adapters
