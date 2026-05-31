#pragma once

#include "trace_graph/activity_record.hpp"

#include <memory>
#include <vector>

namespace TraceGraph {

void normalize_trace_records(std::vector<std::unique_ptr<ActivityRecord>> & records);

} // namespace TraceGraph
