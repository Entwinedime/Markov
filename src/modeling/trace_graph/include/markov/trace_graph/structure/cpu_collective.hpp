#pragma once

#include <cstddef>
#include <map>
#include <span>
#include <vector>

namespace markov::trace_graph::structure {

enum class CpuCollectiveKind { AllReduce, Broadcast };
using CpuCollectiveEntries = std::map<int, std::vector<int>>;

/** Entry dependencies for the supported synchronous Gloo execution rules.
 * All-reduce waits for every participant. Broadcast uses the binomial TCP
 * tree's ancestor and direct-child entries, not parent-return dependencies.
 * members is the validated process-group order; root_index is a valid member
 * position for broadcast and unused for all-reduce. No timings are accepted.
 */
[[nodiscard]] CpuCollectiveEntries cpu_collective_entries(CpuCollectiveKind kind, std::span<const int> members, size_t root_index = 0);

} // namespace markov::trace_graph::structure
