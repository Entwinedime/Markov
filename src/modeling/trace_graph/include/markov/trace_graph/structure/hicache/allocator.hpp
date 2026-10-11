#pragma once

#include "markov/trace_graph/modules/hicache/model/phase_work.hpp"
#include "markov/trace_graph/structure/tensor_program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace markov::trace_graph::structure::hicache {

/** Naive extend writes these regions in order. Empty regions emit no write.
 * Requires validated sequence_tokens >= prefix_tokens and page_size_tokens > 0.
 * Lengths describe index output, not payload copies or measured durations.
 */
struct ExtendPageRegions {
    uint64_t existing_tail_tokens;
    uint64_t new_full_pages;
    uint64_t new_tail_tokens;
};

[[nodiscard]] ExtendPageRegions extend_page_regions(uint64_t sequence_tokens, uint64_t prefix_tokens, uint64_t page_size_tokens);

struct AscendAllocatorInputs {
    std::string sequence_dtype, last_location_dtype, free_page_dtype;
    bool debug_memory_pool;
    std::optional<bool> triton_task_queue_enabled;
    // Empty in older observations: may retain proven unchanged work, but
    // cannot authorize architecture-specific generation.
    std::string device_name;
};

/** Eager non-speculative Decode for the inspected NPU allocator ABI.
 * No source kernel inventory or numerical model is consulted. A missing plan
 * means unsupported input metadata, never a zero-cost/empty allocation.
 * Current native lowering is scoped to the observed Ascend910B family;
 * unknown or other devices require their own execution rules, not these costs.
 * The program describes tensor calls and explicitly supported native launches.
 * Dependencies include data and single-stream order; CPU preparation has its
 * own order and does not synchronize with those device predecessors. Lowering
 * must retain each tensor call's submission and CPU return before advancing
 * the host, rather than interpret a device result as a blocking Python call.
 * Returned indices are int32; when last_loc is int32 and no page is allocated,
 * the final .int() is a host-only no-op. Views/slicing free_pages create no
 * device work. The input dtype is observed deployment/call metadata.
 */
[[nodiscard]] std::optional<std::vector<TensorOperation>>
    ascend_decode_allocator_program(const modules::hicache::model::HiCacheAllocatorWorkItem & call, const AscendAllocatorInputs & inputs);

/** Common extend prefix before either fused or naive allocation: optional
 * device page counting/readback and the target queue merge. This is explicitly
 * a prefix, not a complete allocator or a replacement-ready DAG. The framework
 * caller supplies whether it already computed num_new_pages; current ordinary
 * alloc_for_extend does not. No kernel-preparation cost belongs to this prefix.
 */
[[nodiscard]] std::optional<std::vector<TensorOperation>>
    ascend_extend_allocator_prefix(const modules::hicache::model::HiCacheAllocatorWorkItem & call, const AscendAllocatorInputs & inputs,
                                   const std::string & prefix_dtype, bool page_count_supplied);

/** Conservative whole-call migration within the same declared allocator ABI
 * and resource deployment. Unknown queue/pointer metadata cannot prove reuse.
 * Source identity is established by the caller, never inferred from costs. */
[[nodiscard]] bool allocator_work_unchanged(const modules::hicache::model::HiCacheAllocatorWorkItem & source,
                                            const modules::hicache::model::HiCacheAllocatorWorkItem & target);

// Current Ascend signature: page size, batch upper bound, extend-token upper
// bound and free-pointer byte alignment modulo 16. No cost parameters.
using AllocatorSpecialization = std::array<uint64_t, 4>;

struct AllocatorPreparation {
    std::string status;
    std::optional<AllocatorSpecialization> specialization;
    std::string path = "unknown";
    bool first_load = false;
};

/** Required preparation's semantic work, shared by standalone binding and
 * an enclosing allocator program. Requires a known specialization/path. */
[[nodiscard]] OperationWork allocator_preparation_operation(const AllocatorPreparation & preparation);

/** Complete tensor program for the small-page fused extend branch. The naive
 * branch remains a separate implementation requirement and returns unavailable
 * here. A required preparation is inside this program; callers replacing the
 * whole region must not additionally bind its old standalone preparation slot.
 */
[[nodiscard]] std::optional<std::vector<TensorOperation>>
    ascend_fused_extend_allocator_program(const modules::hicache::model::HiCacheAllocatorWorkItem & call, const AscendAllocatorInputs & inputs,
                                          const std::string & prefix_dtype, bool page_count_supplied, const AllocatorPreparation & preparation);

/** Naive large-page extend: retain the framework's scalar readbacks and
 * Python branch order, including computation whose result is not consumed.
 * Branch predicates use target lengths, never measured costs or source hits.
 * Views are host preparation, while tensor arithmetic/copy and scalar device
 * readbacks remain separate operations. No Triton preparation is required. */
[[nodiscard]] std::optional<std::vector<TensorOperation>>
    ascend_naive_extend_allocator_program(const modules::hicache::model::HiCacheAllocatorWorkItem & call, const AscendAllocatorInputs & inputs,
                                          const std::string & prefix_dtype, bool page_count_supplied);

struct AllocatorPreparationWorkPlan {
    std::string status = "unavailable";
    std::vector<AllocatorPreparation> calls;
    std::map<std::string, uint64_t> blockers;
};

/** Target Ascend preparation history, without measurements or cost queries.
 * supported_pids identifies the inspected allocator ABI, not observed branches.
 * calls includes ordered prelude and formal calls; prelude affects first use.
 * Unknown implementation/geometry remains a formal blocker. The naive branch
 * skips preparation only after implementation support has been established.
 */
[[nodiscard]] AllocatorPreparationWorkPlan plan_allocator_work(const std::set<std::string> & supported_pids,
                                                               const std::vector<modules::hicache::model::HiCacheAllocatorWorkItem> & calls);

} // namespace markov::trace_graph::structure::hicache
