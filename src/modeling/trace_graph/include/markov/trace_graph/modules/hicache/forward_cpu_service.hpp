#pragma once
#include "markov/trace_graph/core/dag_graph.hpp"
#include <nlohmann/json_fwd.hpp>

namespace markov::trace_graph::modules::hicache {

struct ForwardCpuAdjustment {
    size_t node;
    bool gap;
    uint64_t before, after, begin_us, end_us;
};

// Same-base measured overhead, allocated only outside HiCache-owned work.
// Placement within a phase is proportional to eligible source interval width.
std::vector<ForwardCpuAdjustment> plan_forward_cpu_service(
    const core::DagGraph&, const nlohmann::json& steps, const nlohmann::json& comparison);
void attach_forward_cpu_service(core::DagGraph&, const std::vector<ForwardCpuAdjustment>&);

} // namespace markov::trace_graph::modules::hicache
