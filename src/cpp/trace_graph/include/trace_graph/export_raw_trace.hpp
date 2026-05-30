#pragma once

#include "trace_graph/activity_record.hpp"

#include <memory>
#include <string>
#include <vector>

namespace TraceGraph {

void export_raw_trace(const std::string & filename, const std::vector<std::unique_ptr<ActivityRecord>> & records);

} // namespace TraceGraph
