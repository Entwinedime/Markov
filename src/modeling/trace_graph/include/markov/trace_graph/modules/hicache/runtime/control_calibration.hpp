#pragma once
// Independent CPU/control evidence: file admission, costs and portable resource binding.
#include "markov/trace_graph/modules/hicache/runtime/load_execution.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include <nlohmann/json_fwd.hpp>

namespace markov::trace_graph::modules::hicache::runtime {

/** Read independent control evidence at the file boundary. Operation-specific
 * geometry and resource binding remain with the consumer. */
[[nodiscard]] nlohmann::json read_control_calibration(const std::string & path, std::string_view operation);

/** Round a measured nonnegative microsecond cost for the integer simulator.
 * Invalid or missing measurements are errors, not zero-cost operations. */
[[nodiscard]] uint64_t read_control_duration(const nlohmann::json & value);
[[nodiscard]] LoadIndexOperationCost read_load_operation_cost(const nlohmann::json & row);

/** Observe portable write and eviction programs; CPU corrections must already
 * be attached to the source graph. Release-only extraction requires ordinary
 * release observations, not a write operation. Does not consume target data. */
[[nodiscard]] nlohmann::json export_write_calibration(core::DagGraph & graph, uint64_t begin, uint64_t end, bool release_only = false);

/** Bind opaque worker roles using original submission calls on the base main lane. */
void bind_calibration_workers(const nlohmann::json & submissions, const core::DagGraph & base, const simulation::detail::CpuTaskQueues & queues,
                              std::map<std::string, size_t> & roles);

[[nodiscard]] nlohmann::json export_host_template(const core::DagGraph & graph, const HiCacheHostExpansion & plan, const std::map<std::string, size_t> & roles);
/** Bind portable roles directly to active base resources of one rank. Worker
 * resources must witness a base queue. Costs and local dependencies are retained;
 * target execution supplies event completions. Reuse the preparation-stage queue
 * index while CPU nodes and dependencies are unchanged; added device-only resource
 * identities do not invalidate it. Neither input nor base is mutated. */
[[nodiscard]] HiCacheHostExpansion import_host_template(const nlohmann::json & input, const core::DagGraph & base,
                                                        const simulation::detail::CpuTaskQueues & queues, const std::map<std::string, size_t> & roles);

[[nodiscard]] nlohmann::json export_write_template(const core::DagGraph & graph, const HiCacheWriteExpansion & plan,
                                                   const std::map<std::string, size_t> & roles);
[[nodiscard]] HiCacheWriteExpansion import_write_template(const nlohmann::json & input, const core::DagGraph & base,
                                                          const simulation::detail::CpuTaskQueues & queues, const std::map<std::string, size_t> & roles);

} // namespace markov::trace_graph::modules::hicache::runtime
