#pragma once

#include "markov/trace_graph/core/cpu_service_cost.hpp"
#include <filesystem>
#include <istream>
#include <ostream>

namespace markov::trace_graph::io {
// Read service intervals on the declared base's original CPU coordinates.
core::CpuServiceCost read_cpu_service_cost(std::istream& input, const std::filesystem::path& source_manifest);
void write_cpu_service_cost(std::ostream& output, const std::filesystem::path& source_manifest,
                            const core::CpuServiceCost& cost);
}
