#pragma once

#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/simulation/cpu_task_queues.hpp"

#include <cstddef>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>

namespace markov::trace_graph::source_dag {

/** Append independently measurable caller/worker scopes for a nonempty write
 * confirmation. Require one observed acknowledgement and two CPU-only event
 * destruction tasks per batch, in the declared program order. begin_us is the
 * MIN return for nonblocking calls, or blocking call entry, in source time.
 * Blocking device waits are not admitted as CPU service observations.
 * No target data or measured topology is
 * exported as a target execution rule. Unsupported/partial intervals add no
 * fabricated costs; already complete independent scopes remain useful.
 */
void append_write_confirmation_observations(const adapters::sglang::HiCacheSourceIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                            size_t observation, uint64_t begin_us, uint64_t confirmed_batches, bool corrected, nlohmann::json & rows);

} // namespace markov::trace_graph::source_dag
