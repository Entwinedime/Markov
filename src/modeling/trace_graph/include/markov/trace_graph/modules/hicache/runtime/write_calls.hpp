#pragma once
#include "markov/trace_graph/modules/hicache/model/replay.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_host.hpp"
#include "markov/trace_graph/modules/hicache/runtime/writes.hpp"
#include "markov/trace_graph/modules/hicache/runtime/lifecycle_observation.hpp"

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

struct HiCacheLoadBranch {
    size_t owner;
    core::TraceEvent envelope, condition;
    size_t entry_node = 0, return_node = 0;
};
[[nodiscard]] std::vector<HiCacheLoadBranch> observe_load_branches(
    std::span<const core::TraceEvent> observations, std::span<const model::HiCacheReplayFact> facts, uint64_t begin, uint64_t end);
[[nodiscard]] std::vector<HiCacheCapacityGuard> observe_capacity_guards(
    const patch::HiCacheSourceDagIndex & source, std::span<const model::HiCacheReplayFact> facts,
    std::span<const core::TraceEvent> observations, uint64_t begin, uint64_t end);

/** Bind observed host calls to target state operations. Source envelopes locate
 * execution, not target victim counts. Unsupported new host branches stay
 * explicit until their submission and return dependencies can be constructed. */
class HiCacheWriteCalls {
public:
    [[nodiscard]] size_t completion_for(const model::HiCacheDeviceWrite & write) const {
        return completions_.at({write.header.cache_scope, write.header.operation_id});
    }
    HiCacheWriteCalls(model::HiCacheModelReplay & replay, const frontend::HiCacheConfig & config, HiCacheWriteStreamInsertions & insertions)
        : replay_(replay), writes_(replay.state()), stream_insertions_(insertions), page_size_(config.page_size), page_bytes_(config.kv_bytes_per_page), write_back_(config.write_policy == "write_back"),
          empty_write_check_us_(config.io_cost.empty_write_check_us), locked_candidate_us_(config.io_cost.locked_candidate_us), locked_candidate_log2_heap_us_(config.io_cost.locked_candidate_log2_heap_us), write_host_calibration_(config.write_host_calibration),
          release_host_calibration_(config.release_host_calibration), load_index_calibration_(config.load_index_calibration),
          allocator_need_sort_(config.device_allocator_need_sort) {}
    void bind(core::DagGraph & graph, uint64_t begin, uint64_t end);
    void replace_active_load_branches(core::DagGraph & graph);
    // Finalize resource and consumer bindings after all source replacements.
    void rebind_retained_resources(core::DagGraph & graph, const std::map<size_t, size_t> & members);
    void applied(const model::HiCacheReplayFact & input) const;
    /** Returns facts whose allocations completed at this boundary. */
    std::vector<size_t> advance(size_t node, uint64_t time, simulation::FutureDag & future);
    [[nodiscard]] uint64_t duration(size_t node, uint64_t original) const { return writes_.duration(node, original); }
    [[nodiscard]] size_t source_submissions() const { return submissions_; }
    [[nodiscard]] size_t completed_writes() const { return writes_.completed(); }
    [[nodiscard]] size_t resumed_allocations() const { return resumed_; }
    [[nodiscard]] size_t prepared_templates() const { return templates_.size(); }
    [[nodiscard]] size_t prepared_eviction_controls() const { return eviction_controls_.size(); }
    [[nodiscard]] size_t expanded_submissions() const { return expanded_; }
    [[nodiscard]] size_t calibrated_empty_checks() const { return calibrated_empty_checks_; }
    [[nodiscard]] uint64_t calibrated_empty_check_cpu_us() const { return calibrated_empty_check_cpu_us_; }
    [[nodiscard]] size_t generated_control_steps() const { return generated_control_steps_; }
    [[nodiscard]] size_t generated_blocking_checks() const { return generated_blocking_checks_; }
    [[nodiscard]] size_t blocking_check_cost_extrapolations() const { return blocking_check_cost_extrapolations_; }
    [[nodiscard]] size_t phase_extrapolated_control_steps() const { return phase_extrapolated_control_steps_; }

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
    void bind_call_resources(const patch::HiCacheSourceDagIndex & source, Call & call,
                             const HiCacheHostExpansion & plan, bool candidate = false);
    [[nodiscard]] std::vector<HiCacheHostExpansion *> host_plans();
    void bind_capacity_calls(const patch::HiCacheSourceDagIndex & source);
    void replace_source_evictions(core::DagGraph & graph, uint64_t begin, uint64_t end);
    void bind_lifecycle_writes(core::DagGraph & graph);
    void load_write_calibration(core::DagGraph & graph, const simulation::detail::CpuTaskQueues & queues);
    void load_release_calibration(const core::DagGraph & graph, const simulation::detail::CpuTaskQueues & queues);
    void submit_lifecycle_writes(size_t node, uint64_t time, simulation::FutureDag & future);
    void bind_load_attempts(core::DagGraph & graph, uint64_t begin, uint64_t end);
    void bind_load_branches(core::DagGraph & graph, uint64_t begin, uint64_t end);
    void prepare_empty_load_branches(core::DagGraph & graph);
    void prepare_load_admissions(const patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues);
    void prepare_generated_load_admissions(const patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues);
    void start_load_branch(size_t node, uint64_t time, simulation::FutureDag & future);
    void prepare_load_capacity(const patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues);
    std::optional<size_t> expand_capacity(Call & call, const HiCacheFact & fact, simulation::FutureDag & future,
                                         std::optional<size_t> previous = std::nullopt);
    model::HiCacheModelReplay & replay_;
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
    const WriteTemplate & select_write_template(const std::string & pid, const std::string & tid, uint64_t bytes) const;
    // Submit one target write and register its completion. Callers retain
    // ownership of their host continuation and allocation return boundaries.
    HiCacheExpandedWrite submit_target_write(const WriteTemplate & sample, const HiCacheFact & fact,
                                             const model::HiCacheDeviceWrite & operation, HiCacheHostSequence & sequence,
                                             simulation::FutureDag & future);
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
        HiCacheHostExpansion work;
        std::vector<size_t> lanes;
    };
    struct LoadAdmissionProgram {
        std::string pid, tid;
        // Observed aggregate costs have a sampled token count. Primitive costs
        // instead provide a clone operation repeated for target promoted nodes.
        std::optional<uint64_t> tokens;
        std::optional<LoadStep> prefix, retry, clone;
        std::vector<LoadStep> allocation;
        LoadStep tail;
        std::optional<HiCacheHostExpansion> failure;
    };
    std::vector<LoadAdmissionProgram> load_admissions_;
    // Recognized asynchronous clone samples become a per-node operation and
    // a once-per-admission CPU tail; unsupported source geometry stays explicit.
    void split_load_tail(const core::DagGraph & graph, size_t main, LoadAdmissionProgram & program);
    [[nodiscard]] size_t select_load_admission(const std::string & pid, const std::string & tid, uint64_t tokens) const;
    void bind_load_resources(const patch::HiCacheSourceDagIndex & source, Call & call, const LoadAdmissionProgram & program);
    struct LoadBranchReturn { size_t owner, program, continuation; bool expected_allocation; };
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
    std::optional<double> empty_write_check_us_;
    std::optional<double> locked_candidate_us_;
    double locked_candidate_log2_heap_us_ = 0.0;
    std::string write_host_calibration_;
    std::string release_host_calibration_;
    std::string load_index_calibration_;
    bool allocator_need_sort_ = false;
    std::map<std::pair<std::string, std::string>, double> locked_candidate_remainder_us_;
    size_t calibrated_empty_checks_ = 0;
    size_t generated_control_steps_ = 0, phase_extrapolated_control_steps_ = 0;
    size_t generated_blocking_checks_ = 0, blocking_check_cost_extrapolations_ = 0;
    std::map<std::pair<std::string, std::string>, std::pair<double, double>> control_cost_remainders_;
    uint64_t calibrated_empty_check_cpu_us_ = 0;
    std::vector<Call> calls_;
    std::map<size_t, std::vector<std::pair<Action, size_t>>> actions_;
    std::set<size_t> source_owners_, wait_owners_;
    std::map<std::pair<std::string, std::string>, size_t> completions_;
    size_t submissions_ = 0, resumed_ = 0, expanded_ = 0;
};

} // namespace markov::trace_graph::modules::hicache::runtime
