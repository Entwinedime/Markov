/**
 * @file
 * @brief Canonical-radix state machine for target-derived HiCache behavior.
 */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/frontend/model_config.hpp"
#include "markov/trace_graph/modules/hicache/fact.hpp"
#include "markov/trace_graph/modules/hicache/model/result.hpp"
#include "markov/trace_graph/modules/hicache/model/summary.hpp"
#include "markov/trace_graph/modules/hicache/policy.hpp"
#include "markov/trace_graph/modules/hicache/radix/token_radix_tree.hpp"
#include "markov/trace_graph/modules/hicache/router.hpp"
#include "markov/trace_graph/modules/hicache/runtime/async_state.hpp"
#include "markov/trace_graph/modules/hicache/runtime/capacity_index.hpp"
#include "markov/trace_graph/modules/hicache/runtime/ref_ledger.hpp"
#ifdef DEBUG
#include "markov/trace_graph/modules/hicache/runtime/state_index.hpp"
#endif
#include "markov/trace_graph/modules/hicache/runtime/target_control_clock.hpp"
#include "markov/trace_graph/modules/hicache/runtime/target_pager.hpp"
#include "markov/trace_graph/modules/hicache/runtime/token_store.hpp"
#include "markov/trace_graph/modules/hicache/storage/storage_directory.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace markov::trace_graph::modules::hicache::model {

using radix::HiCacheHostEvictionResult;
using radix::HiCacheInsertResult;
using radix::HiCacheNodeId;
#ifdef DEBUG
using radix::HiCacheNodeSplitRecord;
using runtime::hicache_derived_state_mode_name;
using runtime::hicache_sorted_vector;
using runtime::hicache_token_resolution_status_name;
using runtime::HiCacheCapacityAuditIssue;
using runtime::HiCacheCapacityMutation;
using runtime::HiCacheCapacityVictimChoice;
using runtime::HiCacheControlBoundary;
using runtime::HiCacheDerivedStateMode;
using runtime::HiCacheDerivedStateSnapshot;
using runtime::HiCacheDerivedStateView;
using runtime::HiCacheOperationLifecycleTransition;
using runtime::HiCacheRefAuditIssue;
using runtime::HiCacheRefMutation;
#endif
using radix::HiCacheTokenRadixTree;
using runtime::HiCacheAsyncOperationTable;
using runtime::HiCacheBatchTokenResolution;
using runtime::HiCacheCapacityIndex;
using runtime::HiCacheLoadbackOperation;
using runtime::HiCacheOperationHeader;
using runtime::HiCacheOperationKind;
using runtime::HiCacheOperationState;
using runtime::HiCachePagePath;
using runtime::HiCachePrefetchOperation;
using runtime::HiCachePrefetchState;
using runtime::HiCacheRefChange;
using runtime::HiCacheRefLedger;
using runtime::HiCacheStorageOperation;
using runtime::HiCacheTargetControlClock;
using runtime::HiCacheTargetPager;
using runtime::HiCacheTokenDirectory;
using runtime::HiCacheTokenResolution;
using runtime::HiCacheTokenResolutionStatus;
using runtime::HiCacheWritebackOperation;
using storage::HiCacheStorageDirectory;

/**
 * @brief Models target HiCache state over one canonical radix tree per scope.
 *
 * Node residency and reference counters in each scope's radix tree are the sole
 * canonical cache state. Tier membership, dirty/backup/eviction/lock state, and
 * prefetch lifecycle summaries are derived from the tree and asynchronous tables.
 */
class HiCacheState {
public:
    /** @brief Initializes the state machine from an explicit target configuration. */
    explicit HiCacheState(frontend::HiCacheConfig config = frontend::HiCacheConfig{});

    /** @brief Applies one routed fact; only Debug builds populate transition evidence. */
    void apply_fact(const HiCacheFact & fact, HiCacheFactRole role, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions);

    /** @brief Finalizes pending lifecycles after the last fact in the trace. */
    void finalize(HiCacheSummary & summary, HiCacheTransitionBuffer & transitions);

#ifdef DEBUG
    /** @brief Returns the resolved target policy used by this state machine. */
    [[nodiscard]] const HiCacheResolvedPolicyState & resolved_policy() const { return policy_.resolved(); }

    /** @brief Derives a final-state snapshot from canonical runtime structures. */
    [[nodiscard]] HiCacheDerivedStateSnapshot derived_state(HiCacheDerivedStateMode mode = HiCacheDerivedStateMode::MaterializedOnly) const;

    /** @brief Returns the number of owners that still hold lock or host references. */
    [[nodiscard]] uint64_t active_ref_owner_count() const;

    /** @brief Returns the number of radix-node splits. */
    [[nodiscard]] uint64_t radix_split_count() const;

    /** @brief Returns structured radix-split audit records across all scopes. */
    [[nodiscard]] std::vector<HiCacheNodeSplitRecord> radix_split_trace() const;

    /** @brief Returns the number of target-control boundaries observed. */
    [[nodiscard]] uint64_t control_boundary_count() const;

    /** @brief Returns structured target-control boundary records. */
    [[nodiscard]] std::vector<HiCacheControlBoundary> control_boundary_trace() const;

    /** @brief Returns the number of asynchronous operation lifecycle transitions. */
    [[nodiscard]] uint64_t async_lifecycle_transition_count() const;

    /** @brief Returns structured asynchronous lifecycle audit records. */
    [[nodiscard]] std::vector<HiCacheOperationLifecycleTransition> async_lifecycle_trace() const;

    /** @brief Returns the number of target-policy decisions. */
    [[nodiscard]] uint64_t policy_decision_count() const;

    /** @brief Returns decisions that explain acceptance, waiting, truncation, and fallback. */
    [[nodiscard]] const std::vector<HiCachePolicyDecisionRecord> & policy_decision_trace() const { return policy_decisions_; }

    /** @brief Returns the number of pages known to the target storage directory. */
    [[nodiscard]] uint64_t storage_known_page_count() const;

    /** @brief Returns pages readable by host or device through the storage directory. */
    [[nodiscard]] uint64_t storage_readable_page_count() const;

    /** @brief Returns readable pages registered in the target backend hash directory. */
    [[nodiscard]] uint64_t storage_backend_readable_count() const;

    /** @brief Returns storage pages materialized into the host/device radix tree. */
    [[nodiscard]] uint64_t storage_materialized_page_count() const;

    /** @brief Returns the number of capacity-index mutations. */
    [[nodiscard]] uint64_t capacity_mutation_count() const;

    /** @brief Returns the number of recorded capacity-victim selections. */
    [[nodiscard]] uint64_t capacity_victim_choice_count() const;

    /** @brief Returns capacity-index mutation audit records. */
    [[nodiscard]] std::vector<HiCacheCapacityMutation> capacity_mutation_trace() const;

    /** @brief Returns records explaining each capacity-victim selection. */
    [[nodiscard]] std::vector<HiCacheCapacityVictimChoice> capacity_victim_choices() const;

    /** @brief Audits consistency between the capacity index and canonical tree. */
    [[nodiscard]] std::vector<HiCacheCapacityAuditIssue> capacity_audit_issues() const;

    /** @brief Returns the number of reference-ledger mutations. */
    [[nodiscard]] uint64_t ref_mutation_count() const;

    /** @brief Returns reference-lifecycle mutation audit records. */
    [[nodiscard]] std::vector<HiCacheRefMutation> ref_mutation_trace() const;

    /** @brief Audits consistency between the reference ledger and canonical tree. */
    [[nodiscard]] std::vector<HiCacheRefAuditIssue> ref_audit_issues() const;

    /** @brief Produces a stable digest for validation and failure localization. */
    [[nodiscard]] std::string digest() const;
#endif

    /** @brief Exports target-derived effect intents without depending on Debug history. */
    [[nodiscard]] HiCacheEffectIntentCatalog effect_intent_catalog() const;

private:
    /** @brief Request-local lifecycle projection that never owns residency state. */
    struct RequestState {
        uint64_t committed_tokens = 0;
        uint64_t kv_allocated_pages = 0;
        uint64_t cache_protected_pages = 0;
        std::vector<std::string> full_pages;
        std::vector<std::string> device_pages;
        std::vector<std::string> host_pages;
        std::vector<HiCacheNodeId> device_chain;
        std::vector<HiCacheNodeId> host_chain;
        std::string lifecycle_state;
    };

    /** @brief Write-through backup lock awaiting the next control or finalization drain. */
    struct PendingWriteThroughBackup {
        std::string owner;
        std::vector<std::string> pages;
    };

    /**
     * @brief Count-level projection of the SGLang device KV allocator.
     *
     * This ledger only reconstructs the `allocator.available_size()` gate from
     * `free_pages` and `release_pages`. The radix tree still owns logical residency.
     */
    struct DeviceAllocatorLedger {
        bool initialized = false;
        bool need_sort = false;
        uint64_t capacity_pages = 0;
        uint64_t free_pages = 0;
        uint64_t release_pages = 0;

        /** @brief Initializes the allocator projection from target capacity. */
        void configure(uint64_t pages, bool sort_required);

        /** @brief Returns the page count visible to the SGLang allocation gate. */
        [[nodiscard]] uint64_t available_pages() const;

        /** @brief Reports whether an allocation requires device eviction first. */
        [[nodiscard]] bool should_evict(uint64_t requested_pages) const;

        /** @brief Merges pending releases back into the free-page count. */
        void merge_release_pages();

        /** @brief Reconstructs release-queue visibility before an extend boundary. */
        void merge_before_extend(uint64_t extend_tokens, uint64_t batch_size, uint64_t page_size);

        /** @brief Reconstructs release-queue visibility before page allocation. */
        void merge_before_page_allocation(uint64_t requested_pages);

        /** @brief Reports whether the current free count satisfies an allocation. */
        [[nodiscard]] bool can_allocate(uint64_t pages) const;

        /** @brief Consumes free pages and returns the number actually allocated. */
        uint64_t allocate(uint64_t pages);

        /** @brief Adds released pages to the projected release queue. */
        uint64_t release(uint64_t pages);
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
        std::unordered_map<std::string, RequestState> requests;
        std::vector<PendingWriteThroughBackup> pending_write_through_backups;
        uint64_t prefetch_worker_available_ts = 0;
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
        std::string_view reason;
    };

    /** @brief Stable diagnostic labels for one pending-prefetch release boundary. */
    struct PrefetchReleaseReasons {
        std::string_view capacity;
        std::string_view policy;
    };

    /** @brief Semantic identity of one Debug state-transition record. */
    struct TransitionDescriptor {
        std::string_view kind;
        std::string_view tier;
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
     * does not decide whether policy may terminate the prefetch. The current
     * zero-progress approximation is an explicit replacement point for calibration.
     */
    struct PrefetchIoProgressEstimate {
        std::vector<std::string> completed_pages;
#ifdef DEBUG
        std::string reason;
#endif
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
        bool storage_hit_sufficient = false;
        bool terminal_boundary = false;
        bool timeout_elapsed = false;
#ifdef DEBUG
        std::string reason;
#endif
    };

    /** @brief Allocation intent for one request in a batch-level cache extend. */
    struct CacheExtendRequestIntent {
        std::string request_key;
        std::string request_id;
        uint64_t accepted_tokens = 0;
        uint64_t target_device_prefix_tokens = 0;
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
    HiCacheTokenDirectory token_directory_;
    HiCachePolicy policy_;
    std::unordered_map<std::string, ScopedState> scopes_;
#ifdef DEBUG
    uint64_t policy_decision_epoch_ = 0;
    std::vector<HiCachePolicyDecisionRecord> policy_decisions_;
#endif

    /** @brief Normalizes a fact's cache scope, falling back to the configured default. */
    [[nodiscard]] std::string normalized_scope(const HiCacheFact & fact) const;

    /** @brief Builds a scope-qualified request key to prevent cross-scope collisions. */
    [[nodiscard]] std::string scoped_request_key(const HiCacheFact & fact) const;

    /** @brief Returns or creates canonical runtime state for the fact's cache scope. */
    [[nodiscard]] ScopedState & scope_state(const HiCacheFact & fact);

    /** @brief Records token-resolution evidence for Debug input-contract diagnostics. */
    void record_token_resolution(const HiCacheFact & fact, HiCacheSummary & summary, const HiCacheTokenResolution & resolution) const;

    /** @brief Projects a resolved token path into target-sized pages. */
    [[nodiscard]] HiCachePagePath page_path_from_resolution(const HiCacheFact & fact, const HiCacheTokenResolution & resolution) const;

    /** @brief Lazily initializes the device allocator from target configuration. */
    void ensure_device_allocator(ScopedState & scope);
    /** @brief Reports whether a new device page has observable dirty state at insertion. */
    [[nodiscard]] bool inserted_device_dirty_visible_at_insert_boundary() const;
    /** @brief Estimates the page prefix whose storage I/O completed by this boundary. */
    [[nodiscard]] PrefetchIoProgressEstimate estimate_prefetch_io_progress(const HiCachePrefetchOperation & op, const HiCacheFact & fact) const;
    /** @brief Estimates the completed prefix visible at an SGLang termination boundary. */
    [[nodiscard]] PrefetchProgressEstimate estimate_prefetch_progress(const HiCachePrefetchOperation & op, const HiCacheFact & fact,
                                                                      bool terminal_boundary) const;
    /** @brief Reports whether revoke release becomes visible before extend side effects. */
    [[nodiscard]] bool prefetch_release_visible_before_cache_extend(const HiCachePrefetchOperation & op, const HiCacheFact & boundary_fact) const;
    void drain_write_through_backup_refs(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                         const std::string & reason);

    /** @brief Synchronizes tree, reference, and reservation changes into capacity. */
    void sync_capacity(ScopedState & scope, const std::string & cache_scope, const std::vector<HiCacheNodeId> & node_ids, const std::string & reason);

    /** @brief Synchronizes every node affected by one radix insertion. */
    void sync_capacity_for_insert(ScopedState & scope, const std::string & cache_scope, const HiCacheInsertResult & insert, const std::string & reason);

    /** @brief Synchronizes capacity eligibility after a reference mutation. */
    void sync_capacity_for_ref(ScopedState & scope, const std::string & cache_scope, const HiCacheRefChange & change, const std::string & reason);

    /** @brief Records a target-policy decision under a stable global epoch. */
    void record_policy_decision(const HiCacheFact & fact, HiCachePolicyDecisionRecord && decision);

    /** @brief Reports at compile time whether row-level Debug evidence is enabled. */
    [[nodiscard]] static constexpr bool debug_records_enabled() {
#ifdef DEBUG
        return true;
#else
        return false;
#endif
    }

    /** @brief Avoids deriving validation-only state digests in Release builds. */
    [[nodiscard]] std::string debug_state_digest() const {
#ifdef DEBUG
        return digest();
#else
        return {};
#endif
    }

    /** @brief Protects the reusable request prefix described by a cache lookup. */
    void apply_cache_lookup_input(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions);

    /** @brief Allocates batch KV pages and updates request ownership at cache extend. */
    void apply_cache_extend_input(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions);

    /** @brief Resolves every fact-local batch path without request-history fallback. */
    [[nodiscard]] std::optional<std::vector<HiCacheFact>> resolve_cache_extend_entry_facts(const HiCacheFact & fact, HiCacheSummary & summary,
                                                                                           const HiCacheBatchTokenResolution & batch_resolution) const;

    /** @brief Settles prefetch release ordering before cache-extend side effects. */
    void prepare_prefetch_before_cache_extend(const std::vector<HiCacheFact> & entry_facts, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions,
                                              ScopedState & scope);

    /** @brief Drains current-round prefetch release after cache-extend side effects. */
    void drain_prefetch_after_cache_extend(const std::vector<HiCacheFact> & entry_facts, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions,
                                           ScopedState & scope);

    /** @brief Releases request-local protection at a lifecycle commit boundary. */
    void apply_cache_lifecycle_commit(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions);

    /** @brief Starts or advances prefetch state from a candidate anchor. */
    void apply_prefetch_candidate_anchor(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions);

    /** @brief Cancels and releases an older active prefetch for the same request. */
    void suppress_prior_prefetch(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                 const std::string & request_key);

    /** @brief Materializes a completed target prefetch into host radix and storage. */
    void apply_prefetch_ready(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                              HiCachePrefetchOperation & op);
    /** @brief Cancels a prefetch while retaining its not-yet-drained host reservation. */
    void cancel_prefetch_pending_release(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                         HiCachePrefetchOperation & op, const std::string & transition_kind, HiCachePrefetchState prefetch_state);
    /** @brief Settles the request's active prefetch before cache-extend side effects. */
    void settle_prefetch_before_cache_extend(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                             const std::string & request_key);
    /** @brief Releases terminal-prefetch reservation at an explicit scheduler boundary. */
    void drain_prefetch_pending_release(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                        const std::string & request_key, const PrefetchReleaseReasons & reasons);

    /** @brief Updates the request-local committed path and protected-page count. */
    void update_request_state(const HiCacheFact & fact, ScopedState & scope, const std::vector<std::string> & pages);

    /** @brief Inserts a request path into device radix and returns mutation evidence. */
    [[nodiscard]] HiCacheInsertResult insert_request_path(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions,
                                                          ScopedState & scope, const std::vector<std::string> & pages);

    /** @brief Applies the configured host/storage backup policy to a request path. */
    void apply_write_count_policy(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                  const std::vector<std::string> & pages);

    /** @brief Records why hit-count backup is disabled for the current policy. */
    void record_write_count_skip(const HiCacheFact & fact, const std::vector<std::string> & pages, const std::string & reason);

    /** @brief Increments one device node's hit count and applies threshold backup. */
    void apply_write_count_to_node(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                   const WriteCountRequest & request);

    /** @brief Enforces target-derived device capacity before allocation. */
    void enforce_device_capacity(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                 uint64_t requested_pages);

    /** @brief Enforces target-derived host capacity before insertion or reservation. */
    void enforce_host_capacity(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                               uint64_t requested_pages);

    /** @brief Requests host capacity with caller-selected truncation or rejection. */
    [[nodiscard]] HostAllocationResult request_host_allocation(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions,
                                                               ScopedState & scope, const HostAllocationRequest & request);

    /** @brief Evicts one device node and synchronizes all derived projections. */
    [[nodiscard]] uint64_t evict_device_node(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                             HiCacheNodeId node_id);

    /** @brief Records why device eviction does or does not require dirty write-back. */
    void record_device_eviction_policy(const HiCacheFact & fact, const radix::HiCacheCacheNode & node, bool needs_writeback);

    /** @brief Commits and acknowledges one dirty write-back before device eviction. */
    [[nodiscard]] bool commit_device_eviction_writeback(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions,
                                                        ScopedState & scope, HiCacheNodeId node_id, const std::vector<std::string> & pages);

    /** @brief Removes device residency and releases its allocator pages. */
    [[nodiscard]] uint64_t release_device_residency(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions,
                                                    ScopedState & scope, HiCacheNodeId node_id, const std::vector<std::string> & pages);

    /** @brief Evicts one host leaf and synchronizes capacity and transition evidence. */
    [[nodiscard]] uint64_t evict_host_node(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                           HiCacheNodeId node_id);
    /** @brief Marks one host node as backed up and records storage readability. */
    [[nodiscard]] bool commit_host_backup(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                          HiCacheNodeId node_id, bool storage_readable);

    /** @brief Reserves host pages required before a backup can materialize. */
    [[nodiscard]] bool reserve_host_backup_capacity(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions,
                                                    ScopedState & scope, const std::vector<std::string> & pages, uint64_t allocation_pages);

    /** @brief Starts storage backup lifecycle and acquires its host reference. */
    [[nodiscard]] std::string begin_storage_backup(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions,
                                                   ScopedState & scope, HiCacheNodeId node_id, const std::vector<std::string> & pages);

    /** @brief Commits host/storage residency before asynchronous acknowledgement. */
    void materialize_host_backup(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                 HiCacheNodeId node_id, const std::vector<std::string> & pages, bool storage_readable);

    /** @brief Acknowledges storage backup and releases its temporary host reference. */
    void complete_storage_backup(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                 const std::string & storage_id, const std::vector<std::string> & pages);
    /** @brief Holds a write-through reference until the next target-control drain. */
    void hold_write_through_backup_ref(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, ScopedState & scope,
                                       HiCacheNodeId node_id, const std::vector<std::string> & pages);
    /** @brief Emits one Debug-only state transition with optional before/after digests. */
    void record_transition(const HiCacheFact & fact, HiCacheSummary & summary, HiCacheTransitionBuffer & transitions, const TransitionDescriptor & descriptor,
                           const std::vector<std::string> & pages, const std::string & before_digest);
};

/** @brief Runs the HiCache state model and returns effect intents without mutating the DAG. */
[[nodiscard]] HiCacheModelResult apply_hicache_model(core::DagGraph & graph, const frontend::HiCacheConfig & config);

} // namespace markov::trace_graph::modules::hicache::model
