/**
 * @file
 * @brief Canonical-radix state machine for target-derived HiCache behavior.
 */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/frontend/model_config.hpp"
#include "markov/trace_graph/modules/hicache/fact.hpp"
#include "markov/trace_graph/modules/hicache/model/result.hpp"
#include "markov/trace_graph/modules/hicache/policy.hpp"
#include "markov/trace_graph/modules/hicache/radix/token_radix_tree.hpp"
#include "markov/trace_graph/modules/hicache/router.hpp"
#include "markov/trace_graph/modules/hicache/runtime/async_state.hpp"
#include "markov/trace_graph/modules/hicache/runtime/capacity_index.hpp"
#include "markov/trace_graph/modules/hicache/runtime/device_allocator.hpp"
#include "markov/trace_graph/modules/hicache/runtime/ref_ledger.hpp"
#include "markov/trace_graph/modules/hicache/runtime/target_control_clock.hpp"
#include "markov/trace_graph/modules/hicache/runtime/target_pager.hpp"
#include "markov/trace_graph/modules/hicache/runtime/token_store.hpp"
#include "markov/trace_graph/modules/hicache/storage/storage_directory.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace markov::trace_graph::modules::hicache::model {

using radix::HiCacheHostEvictionResult;
using radix::HiCacheInsertResult;
using radix::HiCacheNodeId;
using radix::HiCacheTokenRadixTree;
using runtime::DeviceAllocatorLedger;
using runtime::HiCacheAsyncOperationTable;
using runtime::HiCacheBatchTokenResolution;
using runtime::HiCacheCapacityIndex;
using runtime::HiCacheIoSchedule;
using runtime::HiCacheLoadbackOperation;
using runtime::HiCacheOperationHeader;
using runtime::HiCacheOperationState;
using runtime::HiCachePagePath;
using runtime::HiCachePrefetchOperation;
using runtime::HiCachePrefetchState;
using runtime::HiCacheRefChange;
using runtime::HiCacheRefLedger;
using runtime::HiCacheStorageOperation;
using runtime::HiCacheTargetControlClock;
using runtime::HiCacheTargetPager;
using runtime::HiCacheTokenResolution;
using storage::HiCacheStorageDirectory;

/** @brief Structural work performed by one allocator-driven device cleanup pass. */
struct DeviceCapacityEnforcementResult {
    uint64_t evicted_node_count = 0;
    uint64_t evicted_page_count = 0;
    uint64_t dirty_evicted_node_count = 0;
    uint64_t dirty_evicted_page_count = 0;
};

/** Work selected by target capacity state, including eviction without DMA.
 * The ordered victims are not source observations or timing coefficients. */
struct HiCacheEvictionWork {
    struct Victim {
        HiCacheNodeId node;
        uint64_t page_count;
        bool release_after_write;
        bool backed_up;
    };
    uint64_t requested_pages = 0;
    struct RejectedCandidate {
        HiCacheNodeId node;
        size_t before_victim;
        std::string reason;
        size_t heap_size = 0; // Candidate count before this pop, from predicted state.
    };
    std::vector<RejectedCandidate> rejected_candidates;
    std::vector<Victim> victims;
};

/** Geometry at this lookup, before later requests can split the radix nodes.
 * Each entry is one predicted slice/clone, not a source timing sample. */
struct HiCacheLoadAdmissionWork {
    uint64_t requested_pages = 0;
    std::vector<uint64_t> node_pages;
    bool allocated = false;
};

/** Counts sampled before the scheduler's cross-rank MIN, not a drain result. */
struct HiCachePrefetchQueueSizes {
    uint64_t revoked_operations = 0;
    uint64_t released_pages = 0;
    uint64_t backup_acks = 0;
};

struct HiCachePrefetchCheck {
    bool ongoing = false;
    bool can_stop = true;
    bool terminated = false;
};

/** One start_loading submission. Service belongs to this batch, not separately
 * to each member operation. Device completion and scheduler ACK are distinct.
 */
struct HiCacheLoadbackBatch {
    uint64_t id = 0;
    std::vector<std::string> operations;
    HiCacheIoSchedule io_schedule;
    std::optional<uint64_t> completed_at;
};

struct HiCacheDeviceWrite {
    HiCacheOperationHeader header;
    HiCacheIoSchedule schedule;
    HiCacheNodeId node = 0; // Target radix identity; equal payload sizes are not identities.
};

enum class HiCacheLifecycleExecution { Immediate, DeferReturn, Stepped };

/** Canonical radix residency, references and asynchronous work per cache scope. */
class HiCacheState {
public:
    /** @brief Initializes the state machine from an explicit target configuration. */
    explicit HiCacheState(frontend::HiCacheConfig config = frontend::HiCacheConfig{});
    // Pending continuations refer to this canonical state and its cache scopes.
    HiCacheState(const HiCacheState &) = delete;
    HiCacheState & operator=(const HiCacheState &) = delete;

    /** @brief Registers a future cache-extend fact as the request's prefetch control boundary. */
    void register_prefetch_control_boundary(const HiCacheFact & fact);

    /** Fix source identity before target execution may reorder facts. */
    void register_effect_identity(const HiCacheFact & fact);

    /** Retain cache residency and reset request-local state. Execution-driven
     * prefetch requires explicit query, stop, visibility and scheduler events;
     * it does not infer them from future cache-extend facts or end of trace.
     */
    void begin_formal_window(bool execution_prefetch_control = false);

    /** Execution-driven prefetch: inspect the full queued service, stop at the
     * actual stop call, then publish the cross-rank MIN at its visible boundary.
     * Times share the state's absolute clock. These APIs never read target data.
     * Host release is separate; publication does not free outstanding buffers.
     */
    [[nodiscard]] const HiCachePrefetchOperation * prefetch_operation(const HiCacheFact & fact) const;
    // Background work belongs to the original candidate, not the latest request.
    [[nodiscard]] const HiCachePrefetchOperation * prefetch_candidate_operation(const HiCacheFact & candidate) const;
    // Query samples storage locally; completion consumes the cross-rank hit MIN.
    uint64_t query_prefetch_storage(const HiCacheFact & fact);
    void complete_prefetch_query(const HiCacheFact & fact, uint64_t common_hit_pages);
    [[nodiscard]] HiCachePrefetchCheck sample_prefetch_check(const HiCacheFact & fact) const;
    uint64_t stop_prefetch(const HiCacheFact & fact);
    void publish_prefetch(const HiCacheFact & fact, uint64_t visible_pages);
    void complete_prefetch_io(const HiCacheFact & fact);
    // Background return belongs to an operation, even if its request was replaced.
    void complete_prefetch_io(const HiCacheOperationHeader & operation, uint64_t timestamp_us);
    // Sample revoke/backup-ACK/page-release queues before MIN, drain returned
    // counts later. Refresh storage completion from its current target schedule;
    // new arrivals during MIN stay queued. Sampling never releases host pins.
    [[nodiscard]] HiCachePrefetchQueueSizes prefetch_queue_sizes(const HiCacheFact & fact);
    void drain_prefetch_queues(const HiCacheFact & fact, HiCachePrefetchQueueSizes counts);

    // SGLang's ordinary write() flushes its DMA queue on every call. Count the
    // completed prefix, then acknowledge only the cross-rank MIN at return.
    // Only an actual device callback makes a submitted write complete.
    [[nodiscard]] uint64_t write_completion_count(const HiCacheFact & fact) const;
    [[nodiscard]] std::vector<HiCacheDeviceWrite> unacknowledged_device_writes(const HiCacheFact & fact) const;
    void acknowledge_writes(const HiCacheFact & fact, uint64_t batches);
    [[nodiscard]] std::vector<HiCacheDeviceWrite> pending_device_writes(const HiCacheFact & fact) const;
    void complete_device_write(const HiCacheFact & fact, const std::string & operation_id);
    [[nodiscard]] bool allocation_pending(const HiCacheFact & fact) const;
    [[nodiscard]] const HiCacheEvictionWork * eviction_work(const HiCacheFact & fact) const;
    [[nodiscard]] const HiCacheLoadAdmissionWork * load_admission_work(const HiCacheFact & fact) const;
    /** Read-only outcome after the already selected capacity releases. Does
     * not acknowledge writes, release pages or publish a load operation. */
    [[nodiscard]] bool load_allocation_will_succeed(const HiCacheFact & fact) const;
    /** Acknowledge completed DMA without publishing the waiting allocation. */
    void confirm_allocation_writes(const HiCacheFact & fact);
    void resume_allocation(const HiCacheFact & fact);

    // Flush the pending load() operations at the actual start_loading entry.
    // Only a device-completion callback marks this batch done. MIN consumes
    // batch counts, not operation counts; request completion never releases it.
    // Controller-scoped submission: request identity is unnecessary, including
    // a source-empty start_loading that has queued work in the target state.
    [[nodiscard]] std::optional<HiCacheLoadbackBatch> submit_loadbacks(const HiCacheFact & fact);
    // In-flight operations supplying this request's logical device prefix,
    // including loads owned by another request and not yet submitted loads.
    // Connect their per-layer readiness gates, not the scheduler ACK boundary.
    [[nodiscard]] std::vector<std::string> loadback_dependencies(const HiCacheFact & request) const;
    void complete_loadback_batch(const HiCacheFact & fact, uint64_t batch_id);
    [[nodiscard]] uint64_t load_completion_count(const HiCacheFact & fact) const;
    void acknowledge_loads(const HiCacheFact & fact, uint64_t batches);

    /** @brief Applies one routed fact; only Debug builds populate internal transition evidence. */
    void apply_fact(const HiCacheFact & fact, HiCacheFactRole role, bool observe_effects = true,
                    HiCacheLifecycleExecution lifecycle = HiCacheLifecycleExecution::Immediate);
    [[nodiscard]] bool lifecycle_return_pending(const HiCacheFact & fact) const;
    /** Advance only whole target nodes covered by the ready logical prefix. */
    void advance_lifecycle_insert(const HiCacheFact & fact, uint64_t ready_tokens);
    void complete_lifecycle_return(const HiCacheFact & fact);

    /** @brief Finalizes pending lifecycles after the last fact in the trace. */
    void finalize();


    /** @brief Exports one explicit decision for every registered direct-effect opportunity. */
    [[nodiscard]] HiCacheEffectDecisionLedger effect_decision_ledger() const;

    /** @brief Returns target prefill work accumulated at formal cache-extend boundaries. */
    [[nodiscard]] const std::vector<HiCachePrefillWorkItem> & prefill_work_items() const { return prefill_work_items_; }
    [[nodiscard]] const std::vector<HiCacheAllocatorWorkItem> & allocator_work_items() const { return allocator_work_items_; }

private:
    /** @brief Request-local lifecycle projection that never owns residency state. */
    struct RequestState {
        uint64_t committed_tokens = 0;
        uint64_t extended_tokens = 0;
        uint64_t decoded_tokens = 0;
        uint64_t decode_iteration = 0;
        uint64_t kv_allocated_pages = 0;
        uint64_t cache_protected_pages = 0;
        // Workload lookup boundary, not the source configuration's cache hit count.
        std::optional<uint64_t> lookup_token_limit;
        std::vector<std::string> full_pages;
        std::vector<std::string> device_pages;
        std::vector<std::string> host_pages;
        std::vector<HiCacheNodeId> device_chain;
        std::vector<HiCacheNodeId> host_chain;
        std::string lifecycle_state;
        bool prefetch_candidate_seen = false;
    };

    /** @brief One immediate write()/start_writing() DMA batch awaiting scheduler acknowledgement. */
    struct PendingWriteThroughBackup {
        std::string owner;
        std::string storage_operation_id;
        std::vector<std::string> pages;
    };

    struct PendingEviction {
        HiCacheNodeId node = 0;
        std::vector<std::string> pages;
        std::string owner;
    };
    struct PendingAllocation {
        size_t fact_id = 0;
        std::vector<PendingEviction> victims;
        std::function<void(const HiCacheFact &)> continuation;
    };

    struct PendingLifecycleReturn {
        size_t fact_id;
        std::string request_key, kind;
        uint64_t existing_prefix_pages, protected_pages, extended_pages, committed_pages, aligned_pages, token_count;
        std::vector<std::string> pages;
        uint64_t page_size, last_step_us, ready_tokens = 0;
        radix::HiCacheDeviceInsertCursor cursor;
    };

    /** @brief Complete canonical runtime state for one cache scope. */
    struct ScopedState {
        HiCacheTokenRadixTree tree;
        HiCacheStorageDirectory storage;
        HiCacheAsyncOperationTable async_ops;
        HiCacheCapacityIndex capacity;
        HiCacheRefLedger refs;
        HiCacheTargetControlClock clock;
        DeviceAllocatorLedger device_allocator;
        std::optional<PendingAllocation> allocation;
        std::optional<PendingLifecycleReturn> lifecycle_return;
        std::map<size_t, HiCacheEvictionWork> eviction_work;
        std::map<size_t, HiCacheLoadAdmissionWork> load_admission_work;
        std::unordered_map<std::string, RequestState> requests;
        std::vector<PendingWriteThroughBackup> pending_write_through_backups;
        std::vector<std::string> pending_loadbacks;
        std::deque<HiCacheLoadbackBatch> load_ack_queue;
        std::deque<std::string> prefetch_revoke_queue;
        std::deque<std::string> backup_ack_queue;
        // Run-length page FIFO: operation identity and remaining page count.
        std::deque<std::pair<std::string, uint64_t>> prefetch_host_release_queue;
    };

    /**
     * @brief Result of one host allocation under target capacity.
     *
     * The model intentionally uses capacity and reservation projections instead of
     * a separate host allocator. Write-backup and prefetch paths must pass through
     * this result so rejection, cleanup, and truncation semantics remain consistent.
     */
    struct HostAllocationResult {
        uint64_t requested_pages = 0;
        uint64_t accepted_pages = 0;
        uint64_t capacity_pages = 0;
        uint64_t occupied_pages = 0;
        uint64_t reserved_pages = 0;
        bool accepted = false;
        bool truncated = false;
    };

    /** @brief Semantic request passed through the shared host-capacity gate. */
    struct HostAllocationRequest {
        uint64_t requested_pages = 0;
        uint64_t minimum_pages = 0;
        bool allow_truncate = false;
    };


    /** @brief Node and threshold consumed by one hit-count policy update. */
    struct WriteCountRequest {
        HiCacheNodeId node_id = 0;
        uint64_t threshold = 0;
    };

    /**
     * @brief Independent estimate of storage-prefetch I/O progress.
     *
     * This layer only answers how much of the prefix has reached host memory; it
     * does not decide whether policy may terminate the prefetch. Progress uses only
     * the calibrated host-storage bandwidth and exposes complete contiguous pages.
     */
    struct PrefetchIoProgressEstimate {
        std::vector<std::string> completed_pages;
        uint64_t completed_byte_count = 0;
    };

    /**
     * @brief Progress estimate at one target prefetch boundary.
     *
     * A storage hit proves only that a contiguous prefix exists in L3. Completed
     * pages are the subset already transferred to host and therefore safe to insert
     * into the host radix when the prefetch terminates.
     */
    struct PrefetchProgressEstimate {
        std::vector<std::string> completed_pages;
        uint64_t completed_byte_count = 0;
        uint64_t source_boundary_ts = 0;
        uint64_t policy_stop_ts = 0;
        uint64_t target_boundary_ts = 0;
        uint64_t timeout_deadline_ts = 0;
        uint64_t policy_wait_duration_us = 0;
        bool storage_hit_sufficient = false;
        bool boundary_resolved = false;
        bool io_completed = false;
        bool timed_out = false;
        bool service_consumer_dependency_required = false;
        bool visibility_dependency_required = false;
    };

    /** @brief Allocation intent for one request in a batch-level cache extend. */
    struct CacheExtendRequestIntent {
        std::string request_key;
        std::string request_id;
        uint64_t accepted_tokens = 0;
        uint64_t prior_committed_prefix_tokens = 0;
        uint64_t allocation_prefix_tokens = 0;
        uint64_t extend_tokens = 0;
        uint64_t requested_pages = 0;
        uint64_t allocated_pages = 0;
        std::vector<std::string> full_pages;
        std::vector<std::string> device_pages;
        std::vector<std::string> host_pages;
    };

    /** @brief Batch allocation intent represented by one `cache_extend_input` fact. */
    struct CacheExtendBatchIntent {
        uint64_t batch_size = 0;
        uint64_t total_extend_tokens = 0;
        uint64_t requested_pages = 0;
        uint64_t allocated_pages = 0;
        std::vector<CacheExtendRequestIntent> requests;
    };

    frontend::HiCacheConfig config_;
    HiCacheTargetPager pager_;
    HiCachePolicy policy_;
    std::unordered_map<std::string, ScopedState> scopes_;
    std::unordered_map<std::string, uint64_t> io_lane_available_ts_;
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> prefetch_lane_queue_;
    std::unordered_map<std::string, std::vector<HiCacheFact>> prefetch_control_boundaries_;
    std::vector<HiCacheEffectOpportunity> effect_opportunities_;
    std::vector<HiCachePrefillWorkItem> prefill_work_items_;
    std::vector<HiCacheAllocatorWorkItem> allocator_work_items_;
    std::unordered_map<std::string, uint64_t> effect_fact_ordinals_;
    std::map<std::pair<size_t, std::string>, uint64_t> source_effect_ordinals_;
    std::unordered_map<std::string, std::string> effect_scope_identities_;
    uint64_t effect_opportunity_epoch_ = 0;
    uint64_t effect_scope_epoch_ = 0;
    bool formal_window_active_ = false;
    bool formal_boundary_seen_ = false;
    bool execution_prefetch_control_ = false;

    /** @brief Normalizes a fact's cache scope, falling back to the configured default. */
    [[nodiscard]] std::string normalized_scope(const HiCacheFact & fact) const;

    /** @brief Builds a scope-qualified request key to prevent cross-scope collisions. */
    [[nodiscard]] std::string scoped_request_key(const HiCacheFact & fact) const;

    /** @brief Finds the first registered cache-extend boundary after this lookup. */
    [[nodiscard]] const HiCacheFact * prefetch_control_boundary_for_lookup(const HiCacheFact & fact) const;

    /** @brief Finds the first registered cache-extend boundary after an operation was enqueued. */
    [[nodiscard]] const HiCacheFact * prefetch_control_boundary_for_operation(const HiCachePrefetchOperation & operation) const;

    /** @brief Returns or creates canonical runtime state for the fact's cache scope. */
    [[nodiscard]] ScopedState & scope_state(const HiCacheFact & fact);

    /** @brief Registers the fixed direct-effect opportunities owned by one input fact. */
    void observe_effect_opportunities(const HiCacheFact & fact, HiCacheFactRole role);
    [[nodiscard]] const std::string & effect_scope_identity(const HiCacheFact & fact);


    /** @brief Projects a resolved token path into target-sized pages. */
    [[nodiscard]] HiCachePagePath page_path_from_resolution(const HiCacheFact & fact, const HiCacheTokenResolution & resolution) const;

    /** @brief Lazily initializes the device allocator from target configuration. */
    void ensure_device_allocator(ScopedState & scope);
    /** @brief Reports whether a new device page has observable dirty state at insertion. */
    [[nodiscard]] bool inserted_device_dirty_visible_at_insert_boundary() const;
    /** @brief Use the same service batches, costs and resource scopes as the DAG. */
    [[nodiscard]] HiCacheIoSchedule schedule_target_io(const std::string & scope, const std::string & kind, uint64_t eligibility_ts, uint64_t page_count,
                                                       std::span<const uint64_t> existing_batch_pages = {}, std::optional<uint64_t> stop_ts = std::nullopt);
    /** @brief Publishes D2H/H2S state whose shared service clock reached this boundary. */
    void advance_storage_backups(const HiCacheFact & fact, ScopedState & scope, bool force = false);
    /** @brief Materializes every target prefetch completed by the current global boundary. */
    void advance_ready_prefetches(const HiCacheFact & fact);
    /** @brief Binds a transfer-owned prefetch dependency to its canonical cache consumer. */
    void bind_prefetch_consumer_boundary(const HiCacheFact & fact, ScopedState & scope, HiCachePrefetchOperation & op);
    /** @brief Estimates the page prefix whose storage I/O completed by this boundary. */
    [[nodiscard]] PrefetchIoProgressEstimate estimate_prefetch_io_progress(const HiCachePrefetchOperation & op, uint64_t boundary_ts) const;
    /** @brief Resolves target progress and control timing from one source cache-extend boundary. */
    [[nodiscard]] PrefetchProgressEstimate estimate_prefetch_progress(const HiCachePrefetchOperation & op, const HiCacheFact & source_boundary) const;
    void drain_write_through_backup_refs(const HiCacheFact & fact, ScopedState & scope, std::optional<uint64_t> acknowledged_batches = std::nullopt);
    void submit_storage_backup(const HiCacheFact & fact, ScopedState & scope, HiCacheStorageOperation & operation, uint64_t submitted_at);

    /** @brief Synchronizes tree, reference, and reservation changes into capacity. */
    void sync_capacity(ScopedState & scope, const std::string & cache_scope, const std::vector<HiCacheNodeId> & node_ids);

    /** @brief Synchronizes every node affected by one radix insertion. */
    void sync_capacity_for_insert(ScopedState & scope, const std::string & cache_scope, const HiCacheInsertResult & insert);

    /** @brief Synchronizes capacity eligibility after a reference mutation. */
    void sync_capacity_for_ref(ScopedState & scope, const std::string & cache_scope, const HiCacheRefChange & change);


    /** @brief Protects the reusable request prefix described by a cache lookup. */
    void apply_cache_lookup_input(const HiCacheFact & fact);

    /** @brief Allocates batch KV pages and updates request ownership at cache extend. */
    void apply_cache_extend_input(const HiCacheFact & fact);
    void apply_cache_decode_allocation(const HiCacheFact & fact);

    /** @brief Resolves every fact-local batch path without request-history fallback. */
    [[nodiscard]] std::optional<std::vector<HiCacheFact>> resolve_cache_extend_entry_facts(const HiCacheFact & fact,
                                                                                           const HiCacheBatchTokenResolution & batch_resolution);

    /** @brief Settles prefetch release ordering before cache-extend side effects. */
    void prepare_prefetch_before_cache_extend(const std::vector<HiCacheFact> & entry_facts, ScopedState & scope);

    /** @brief Drains current-round prefetch release after cache-extend side effects. */
    void drain_prefetch_after_cache_extend(const std::vector<HiCacheFact> & entry_facts, ScopedState & scope);

    /** @brief Releases request-local protection at a lifecycle commit boundary. */
    void apply_cache_lifecycle_commit(const HiCacheFact & fact, HiCacheLifecycleExecution execution);

    /** @brief Starts or advances prefetch state from a candidate anchor. */
    void apply_prefetch_candidate_anchor(const HiCacheFact & fact);
    void queue_prefetch_host_release(ScopedState & scope, const HiCachePrefetchOperation & op, uint64_t pages);

    /** @brief Cancels and releases an older active prefetch for the same request. */
    void suppress_prior_prefetch(const HiCacheFact & fact, ScopedState & scope, const std::string & request_key);

    /** @brief Materializes a completed target prefetch into host radix and storage. */
    void apply_prefetch_ready(const HiCacheFact & fact, ScopedState & scope, HiCachePrefetchOperation & op);
    /** @brief Cancels a prefetch while retaining its not-yet-drained host reservation. */
    void cancel_prefetch_pending_release(const HiCacheFact & fact, ScopedState & scope, HiCachePrefetchOperation & op, HiCachePrefetchState prefetch_state);
    /** @brief Settles the request's active prefetch before cache-extend side effects. */
    void settle_prefetch_before_cache_extend(const HiCacheFact & fact, ScopedState & scope, const std::string & request_key);
    /** @brief Releases terminal-prefetch reservation at an explicit scheduler boundary. */
    void drain_prefetch_pending_release(const HiCacheFact & fact, ScopedState & scope, const std::string & request_key);

    /** @brief Updates the request-local committed path and protected-page count. */
    void update_request_state(const HiCacheFact & fact, ScopedState & scope, const std::vector<std::string> & pages);

    /** @brief Applies the configured host/storage backup policy to a request path. */
    void apply_write_count_policy(const HiCacheFact & fact, ScopedState & scope, const HiCacheInsertResult & insert);


    /** @brief Increments one device node's hit count and applies threshold backup. */
    void apply_write_count_to_node(const HiCacheFact & fact, ScopedState & scope, const WriteCountRequest & request);

    /** @brief Enforces target-derived device capacity before allocation. */
    DeviceCapacityEnforcementResult enforce_device_capacity(const HiCacheFact & fact, ScopedState & scope, uint64_t requested_pages);
    void allocate_after_capacity(const HiCacheFact & fact, ScopedState & scope, uint64_t requested_pages,
                                 std::function<void(const HiCacheFact &)> continuation);

    /** @brief Enforces target-derived host capacity before insertion or reservation. */
    void enforce_host_capacity(const HiCacheFact & fact, ScopedState & scope, uint64_t requested_pages);

    /** @brief Requests host capacity with caller-selected truncation or rejection. */
    [[nodiscard]] HostAllocationResult request_host_allocation(const HiCacheFact & fact, ScopedState & scope, const HostAllocationRequest & request);

    /** @brief Evicts one device node and synchronizes all derived projections. */
    [[nodiscard]] uint64_t evict_device_node(const HiCacheFact & fact, ScopedState & scope, HiCacheNodeId node_id);


    /** @brief Commits and acknowledges one dirty write-back before device eviction. */
    [[nodiscard]] bool commit_device_eviction_writeback(const HiCacheFact & fact, ScopedState & scope, HiCacheNodeId node_id,
                                                        const std::vector<std::string> & pages);

    /** @brief Removes device residency and releases its allocator pages. */
    [[nodiscard]] uint64_t release_device_residency(const HiCacheFact & fact, ScopedState & scope, HiCacheNodeId node_id,
                                                    const std::vector<std::string> & pages);

    /** @brief Evicts one host leaf and synchronizes capacity and transition evidence. */
    [[nodiscard]] uint64_t evict_host_node(const HiCacheFact & fact, ScopedState & scope, HiCacheNodeId node_id);
    /** @brief Marks one host node as backed up and records storage readability. */
    [[nodiscard]] bool commit_host_backup(const HiCacheFact & fact, ScopedState & scope, HiCacheNodeId node_id, bool storage_readable);

    /** @brief Reserves host pages required before a backup can materialize. */
    [[nodiscard]] bool reserve_host_backup_capacity(const HiCacheFact & fact, ScopedState & scope, uint64_t allocation_pages);

    /** @brief Starts storage backup lifecycle and acquires its host reference. */
    [[nodiscard]] std::string begin_storage_backup(const HiCacheFact & fact, ScopedState & scope, HiCacheNodeId node_id, const std::vector<std::string> & pages,
                                                   const std::vector<std::string> & device_to_host_pages);

    /** @brief Publishes the host copy after D2H acknowledgement. */
    void materialize_host_backup(const HiCacheFact & fact, ScopedState & scope, HiCacheNodeId node_id, const std::vector<std::string> & pages,
                                 const std::string & storage_id);

    /** @brief Publishes storage readability after H2S acknowledgement. */
    void complete_storage_backup(const HiCacheFact & fact, ScopedState & scope, const std::string & storage_id);
    /** @brief Holds a write-through reference until the next target-control drain. */
    void hold_write_through_backup_ref(const HiCacheFact & fact, ScopedState & scope, HiCacheNodeId node_id, const std::vector<std::string> & pages,
                                       const std::string & storage_operation_id);
};

/** Execution times use the same absolute microsecond origin as prelude facts.
 * Called once per valid formal/tail model fact, never for source-actual outcomes.
 * The caller supplies every time or reports a missing execution boundary; it
 * must not substitute source time for a missing target boundary.
 */
using HiCacheFactClock = std::function<uint64_t(const HiCacheFact &)>;

/** Runs the one state model without mutating the DAG. An empty clock retains
 * the existing source-timed replay; supplying a clock does not by itself solve
 * the state/DAG scheduling loop or replace prefetch control semantics.
 */
[[nodiscard]] HiCacheModelResult apply_hicache_model(core::DagGraph & graph, const frontend::HiCacheConfig & config,
                                                     const HiCacheFactClock & execution_clock = {});

} // namespace markov::trace_graph::modules::hicache::model
