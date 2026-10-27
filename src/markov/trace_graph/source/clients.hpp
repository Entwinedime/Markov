#pragma once

#include "markov/trace_graph/source/observations.hpp"

#include <filesystem>
#include <vector>

namespace markov::trace_graph::source {

struct ClientProgram {
    std::vector<RequestWindow> requests;
    std::vector<WorkloadBarrier> barriers;
    std::size_t record_count;
};

// Keep declared control semantics, not observed barrier durations. Diagnostic
// checkpoints are instrumentation, never target scheduling or cache assertions.
ClientProgram read_client_program(const std::filesystem::path & path, Index file);

} // namespace markov::trace_graph::source
