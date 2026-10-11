#pragma once

#include "markov/trace_graph/core/cpu_service_cost.hpp"
#include <filesystem>
#include <istream>
#include <nlohmann/json_fwd.hpp>
#include <ostream>

namespace markov::trace_graph::io {
// Read service intervals on the declared base's original CPU coordinates.
core::CpuServiceCost read_cpu_service_cost(std::istream & input, const std::filesystem::path & source_manifest);
// Both original manifests identify paired CPU measurement provenance. Optional
// paired I/O evidence accompanies the spans; the simulator applies CPU spans only.
// measurement_window declares the extraction scope, not full per-node coverage.
void write_cpu_service_cost(std::ostream & output, const std::filesystem::path & source_manifest, const core::CpuServiceCost & cost,
                            const std::filesystem::path & light_manifest, const nlohmann::json & reference_io, const nlohmann::json & measurement_window);
} // namespace markov::trace_graph::io
