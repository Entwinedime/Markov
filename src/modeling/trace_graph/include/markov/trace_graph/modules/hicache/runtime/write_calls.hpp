#pragma once
#include "markov/trace_graph/cost/hicache_io.hpp"
#include "markov/trace_graph/cost/operator_model.hpp"
#include "markov/trace_graph/modules/hicache/model/replay.hpp"
#include "markov/trace_graph/modules/hicache/runtime/lifecycle_observation.hpp"
#include "markov/trace_graph/modules/hicache/runtime/load_execution.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_host.hpp"
#include "markov/trace_graph/modules/hicache/runtime/writes.hpp"
#include "markov/trace_graph/source_dag/load_admission.hpp"
#include "markov/trace_graph/structure/hicache/resources.hpp"

namespace markov::trace_graph::modules::hicache::runtime {

/** Source call ownership from explicit envelopes and same-request inputs. */
[[nodiscard]] size_t observe_write_call_owner(const patch::HiCacheSourceDagIndex & source, std::span<const model::HiCacheReplayFact> facts,
                                              const std::string & pid, const std::string & tid, uint64_t begin, uint64_t end);

struct HiCacheCapacityGuard {
    size_t owner;
    std::string pid, tid;
    uint64_t begin, end;
    size_t entry_node = 0, return_node = 0;
};

using source_dag::HiCacheLoadBranch;
[[nodiscard]] std::vector<HiCacheCapacityGuard> observe_capacity_guards(const patch::HiCacheSourceDagIndex & source,
                                                                        std::span<const model::HiCacheReplayFact> facts,
                                                                        std::span<const core::TraceEvent> observations, uint64_t begin, uint64_t end);

/** Bind observed host calls to target state operations. Source envelopes locate
 * execution, not target victim counts. Unsupported new host branches stay
 * explicit until their submission and return dependencies can be constructed. */
class HiCacheWriteCalls {
public:
    [[nodiscard]] size_t completion_for(const model::HiCacheDeviceWrite & write) const {
        return completions_.at({ write.header.cache_scope, write.header.operation_id });
    }
    HiCacheWriteCalls(model::HiCacheModelReplay & replay, const frontend::HiCacheConfig & config, HiCacheWriteStreamInsertions & insertions,
                      const structure::hicache::SchedulerComputeResources & compute_resources, cost::OperatorCostProvider & provider)
        : replay_(replay),
          writes_(replay.state()),
          stream_insertions_(insertions),
          page_size_(config.page_size),
          page_bytes_(config.kv_bytes_per_page),
          write_back_(config.write_policy == "write_back"),
          allocator_need_sort_(config.device_allocator_need_sort),
          operator_provider_(provider),
          io_completion_(cost::io_completion_provider(config.io_cost)),
          compute_resources_(compute_resources) {}
    // A crossing source call keeps only its exact retained CPU suffix. Its
    // entry follows initial lifecycle state at the HTTP availability gate;
    // clipped submissions identify removal ownership, never reusable costs.
    void bind(core::DagGraph & graph, uint64_t begin, uint64_t end, size_t opening_node);
    void replace_active_load_branches(core::DagGraph & graph);
    // Finalize resource and consumer bindings after all source replacements.
    void rebind_retained_resources(const patch::HiCacheSourceDagIndex & source, const std::map<size_t, size_t> & members);
    void applied(const model::HiCacheReplayFact & input) const;
    /** Returns facts whose allocations completed at this boundary. */
    std::vector<size_t> advance(size_t node, uint64_t time, simulation::FutureDag & future);
    [[nodiscard]] size_t source_submissions() const { return submissions_; }
    [[nodiscard]] size_t completed_writes() const { return writes_.completed(); }
    [[nodiscard]] size_t resumed_allocations() const { return resumed_; }
    [[nodiscard]] size_t prepared_templates() const { return templates_.size(); }
    [[nodiscard]] size_t prepared_eviction_controls() const { return eviction_controls_.size(); }
    [[nodiscard]] size_t expanded_submissions() const { return expanded_; }
    [[nodiscard]] size_t generated_control_steps() const { return generated_control_steps_; }
    [[nodiscard]] size_t generated_blocking_checks() const { return generated_blocking_checks_; }

private:
    enum class Action { Submit, Wait, Return };
    struct Call {
        size_t owner = 0;
        std::optional<size_t> source;
        size_t return_node = 0;
        bool waiting = false;
        std::string pid, tid;
        uint64_t at_us = 0;
        size_t entry_node = 0;
        std::vector<size_t> successors;
        std::map<size_t, HiCacheWriteStreamPosition> positions;
        std::string lane;
        bool retained_allocation_return = false;
    };
    // Candidate load/capacity resources are required only when target work uses
    // them. Other calls fail immediately if their observed resource is unbound.
    void bind_call_resources(const patch::HiCacheSourceDagIndex & source, Call & call, const HiCacheHostExpansion & plan, bool candidate = false);
    [[nodiscard]] std::vector<HiCacheHostExpansion *> host_plans();
    void bind_capacity_calls(const patch::HiCacheSourceDagIndex & source);
    void replace_source_evictions(core::DagGraph & graph, uint64_t begin, uint64_t end);
    void replace_source_write_calls(core::DagGraph & graph);
    void bind_lifecycle_writes(core::DagGraph & graph, uint64_t begin_us);
    void prepare_write_resources(core::DagGraph & graph, const simulation::detail::CpuTaskQueues & queues);
    void submit_lifecycle_writes(size_t node, uint64_t time, simulation::FutureDag & future);
    void bind_load_attempts(core::DagGraph & graph, uint64_t begin, uint64_t end);
    void bind_load_branches(core::DagGraph & graph, uint64_t begin, uint64_t end);
    void prepare_empty_load_branches(core::DagGraph & graph);
    void prepare_generated_load_admissions(const patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues);
    void start_load_branch(size_t node, uint64_t time, simulation::FutureDag & future);
    std::optional<size_t> expand_capacity(Call & call, const HiCacheFact & fact, simulation::FutureDag & future, std::optional<size_t> previous = std::nullopt);
    model::HiCacheModelReplay & replay_;
    const core::DagGraph * graph_ = nullptr;
    HiCacheWrites writes_;
    HiCacheWriteStreamInsertions & stream_insertions_;
    struct WriteTemplate {
        std::optional<bool> write_back;
        size_t compute_record = 0;
        HiCacheWriteExpansion expansion;
        std::string pid, tid;
        std::vector<size_t> position_lanes;
        bool independent = false;
    };
    std::map<size_t, WriteTemplate> templates_;
    struct WriteResources {
        size_t compute, worker, transfer;
        std::vector<size_t> position_lanes;
    };
    std::map<std::pair<std::string, std::string>, WriteResources> write_resources_;
    // Submit one target write and register its completion. Callers retain
    // ownership of their host continuation and allocation return boundaries.
    HiCacheExpandedWrite submit_target_write(const WriteResources & resources, const HiCacheFact & fact, const model::HiCacheDeviceWrite & operation,
                                             HiCacheHostSequence & sequence, simulation::FutureDag & future);
    struct EvictionControl {
        std::string pid, tid;
        HiCacheEvictionRegion region;
        HiCacheHostExpansion expansion;
        std::vector<size_t> position_lanes;
        bool independent = false;
    };
    std::vector<EvictionControl> eviction_controls_;
    std::vector<HiCacheCapacityGuard> capacity_guards_;
    std::vector<HiCacheLifecycleInsert> lifecycle_inserts_;
    std::map<size_t, std::vector<Call>> lifecycle_writes_at_;
    std::set<size_t> lifecycle_write_owners_;
    struct LoadAttempt {
        size_t owner, entry_node;
        core::TraceEvent event;
        size_t return_node = 0;
    };
    std::vector<LoadAttempt> load_attempts_;
    std::vector<HiCacheLoadBranch> load_branches_;
    struct EmptyLoadBranch {
        size_t owner, continuation;
        HiCacheHostExpansion original;
    };
    std::map<size_t, EmptyLoadBranch> empty_load_branches_;
    struct LoadStep {
        GeneratedDeviceOperation program;
        std::string operation;
        std::vector<size_t> lanes;
    };
    struct LoadAdmissionProgram {
        std::string pid, tid;
        // Cost-free resource programs; target state owns sizes/cardinality.
        std::optional<LoadStep> clone;
        std::vector<LoadStep> allocation;
        LoadStep merge_concat, merge_sort;
        LoadStep prefix_concat;
        std::string main_lane;
    };
    std::vector<LoadAdmissionProgram> load_admissions_;
    // Cost selection never changes allocation or clone execution rules.
    [[nodiscard]] size_t select_load_admission(const std::string & pid, const std::string & tid) const;
    void bind_load_resources(const patch::HiCacheSourceDagIndex & source, Call & call, const LoadAdmissionProgram & program);
    [[nodiscard]] HiCacheHostExpansion cost_load_step(const LoadStep & step, uint64_t pages, bool first_clone = false) const;
    [[nodiscard]] HiCacheHostExpansion bind_load_operation(const LoadStep & step, structure::OperationWork work) const;
    [[nodiscard]] HiCacheHostExpansion load_prefix_concat(const LoadAdmissionProgram & program, const model::HiCacheLoadAdmissionWork & work) const;
    [[nodiscard]] HiCacheHostExpansion load_admission_tail(const LoadAdmissionProgram & program, uint64_t pages, bool allocated) const;
    struct LoadBranchReturn {
        size_t owner, program, continuation;
        bool expected_allocation;
    };
    std::map<size_t, LoadBranchReturn> load_branch_returns_;
    std::map<size_t, HiCacheHostExpansion> load_failure_templates_;
    std::map<size_t, std::vector<size_t>> allocation_returns_;
    std::set<size_t> guarded_owners_;
    std::map<size_t, Call> capacity_calls_;
    std::map<size_t, size_t> capacity_at_, capacity_ack_;
    std::map<size_t, std::map<size_t, std::string>> capacity_position_errors_;
    uint64_t page_size_;
    uint64_t page_bytes_;
    bool write_back_;
    bool allocator_need_sort_ = false;
    cost::OperatorCostProvider & operator_provider_;
    structure::hicache::IoCompletionProvider io_completion_;
    const structure::hicache::SchedulerComputeResources & compute_resources_;
    size_t generated_control_steps_ = 0;
    size_t generated_blocking_checks_ = 0;
    std::map<std::pair<std::string, std::string>, std::pair<double, double>> control_cost_remainders_;
    std::vector<Call> calls_;
    std::map<size_t, std::vector<std::pair<Action, size_t>>> actions_;
    std::set<size_t> source_owners_, wait_owners_;
    std::map<std::pair<std::string, std::string>, size_t> completions_;
    size_t submissions_ = 0, resumed_ = 0, expanded_ = 0;
};

} // namespace markov::trace_graph::modules::hicache::runtime
