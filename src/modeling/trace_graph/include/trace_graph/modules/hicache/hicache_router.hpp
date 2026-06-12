#pragma once

#include "trace_graph/modules/hicache/hicache_fact.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace TraceGraph {

enum class HiCacheFactRole {
    Unknown,
    RequestBoundMatchAnchor,
    RequestLifecycleAnchor,
    RequestAdmission,
    PrefetchDecision,
    PrefetchCheckPoint,
    DiagnosticStateInjection,
};

struct HiCacheFactRoute {
    bool model_fact = false;
    bool known_role = false;
    HiCacheFactRole role = HiCacheFactRole::Unknown;
};

bool is_hicache_state_model_fact(const HiCacheFact & fact);
HiCacheFactRole parse_hicache_fact_role(const std::string & role);
std::string hicache_fact_role_name(HiCacheFactRole role);
HiCacheFactRoute route_hicache_fact(const HiCacheFact & fact);
bool hicache_fact_role_implemented(HiCacheFactRole role);
std::vector<std::string> hicache_required_fact_errors(const HiCacheFact & fact, HiCacheFactRole role, uint64_t effective_page_size);

} // namespace TraceGraph
