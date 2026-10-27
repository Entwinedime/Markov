#pragma once

#include "markov/trace_graph/source/observations.hpp"

#include <filesystem>
#include <vector>

namespace markov::trace_graph::source {

// The client report belongs to the selected base capture, not target truth.
// Keep preparation requests and formal membership in their original order.
std::vector<RequestWindow> read_client_requests(const std::filesystem::path & path);

} // namespace markov::trace_graph::source
