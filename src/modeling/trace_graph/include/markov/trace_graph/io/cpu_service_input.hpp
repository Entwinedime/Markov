#pragma once

#include "markov/trace_graph/core/cpu_service_cost.hpp"
#include <filesystem>
#include <istream>
#include <nlohmann/json_fwd.hpp>
#include <ostream>

namespace markov::trace_graph::io {
// Read service intervals on the declared base's original CPU coordinates.
core::CpuServiceCost read_cpu_service_cost(std::istream & input, const std::filesystem::path & source_manifest);
// Optional paired I/O measurements accompany the CPU spans for model building;
// the simulator reads only CPU spans and does not apply I/O costs a second time.
void write_cpu_service_cost(std::ostream & output, const std::filesystem::path & source_manifest, const core::CpuServiceCost & cost,
                            const nlohmann::json & reference_io);
} // namespace markov::trace_graph::io
