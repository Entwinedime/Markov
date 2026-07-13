/**
 * @file
 * @brief Target-derived HiCache prefetch, writeback, loadback, and storage operations.
 */
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

/**
 * @brief Kind of target-derived cache operation.
 *
 * SGLang gives these operations distinct control paths, while they share identity,
 * scope, request ownership, affected nodes/pages, and lifecycle boundaries.
 */
enum class HiCacheOperationKind : std::uint8_t { Prefetch, Writeback, Loadback, Storage };

/**
 * @brief Common operation lifecycle state.
 *
 * The current model folds acknowledgements synchronously, but explicit states preserve
 * physical intent rather than reducing each operation to an anonymous page mutation.
 */
enum class HiCacheOperationState : std::uint8_t { Created, Queued, Ready, Completed, Committed, Cancelled };

/**
 * @brief Modeled outcome produced by prefetch stop policy.
 *
 * This state drives final-state derivation and prefetch policy. The common operation
 * lifecycle remains in `HiCacheOperationHeader::state`.
 */
enum class HiCachePrefetchState : std::uint8_t { Pending, Ready, Applied, Suppressed, Late, Revoked };

/**
 * @brief Target-derived header shared by every cache operation.
 *
 * The header carries identity, ownership, affected pages/nodes, and the timestamps needed
 * by effect-intent generation. Prefetch hit prefixes and host reservations belong in the
 * concrete operation type rather than this shared contract.
 */
struct HiCacheOperationHeader {
    std::string operation_id;
    HiCacheOperationKind kind = HiCacheOperationKind::Prefetch;
    std::string cache_scope;
    std::string request_key;
    std::string request_id;
    std::string owner;
    std::vector<std::string> pages;
    HiCacheOperationState state = HiCacheOperationState::Created;
    uint64_t enqueue_epoch = 0;
    uint64_t boundary_epoch = 0;
    uint64_t complete_epoch = 0;
    uint64_t enqueue_ts = 0;
    uint64_t boundary_ts = 0;
    uint64_t complete_ts = 0;
    uint64_t consumer_epoch = 0;
    uint64_t consumer_ts = 0;
    size_t consumer_source_node_id = 0;
    size_t consumer_source_event_index = 0;
    std::string consumer_source_fact_role;
    bool consumer_source_available = false;
    size_t source_node_id = 0;
    size_t source_event_index = 0;
    uint64_t source_fact_seq_no = 0;
    std::string source_fact_role;
    std::string source_token_path_id;
    uint64_t source_token_begin = 0;
    uint64_t source_token_end = 0;
};

/** @brief Target-derived timing for one transfer on a logical I/O resource lane. */
struct HiCacheIoSchedule {
    bool available = false;
    std::string resource_lane;
    uint64_t effective_byte_count = 0;
    uint64_t duration_us = 0;
    uint64_t start_ts = 0;
    uint64_t ready_ts = 0;
};

#ifdef DEBUG
/**
 * @brief Debug record for one operation lifecycle transition.
 *
 * This is the async table's own audit trail. It differs from a HiCache state transition:
 * the latter describes residency/reference mutations, while this record explains how an
 * operation advanced.
 */
struct HiCacheOperationLifecycleTransition {
    std::string operation_id;
    HiCacheOperationKind kind = HiCacheOperationKind::Prefetch;
    HiCacheOperationState from_state = HiCacheOperationState::Created;
    HiCacheOperationState to_state = HiCacheOperationState::Created;
    uint64_t transition_epoch = 0;
    uint64_t transition_ts = 0;
    std::string cache_scope;
    std::string request_key;
    std::string reason;
};
#endif

/**
 * @brief One storage-to-host prefetch operation.
 *
 * `planned_pages` is the aligned request, `hit_pages` is the contiguous storage-hit
 * prefix, `payload_transfer_issued` records the SGLang threshold gate that submits that
 * prefix to the payload worker, `completed_pages` is the prefix visible at a termination
 * boundary, and `completed_byte_count` records the target payload service completed by
 * that boundary. `reserved_host_pages` is the L2 reservation accepted after cleanup.
 */
struct HiCachePrefetchOperation {
    HiCacheOperationHeader header;
    std::vector<std::string> host_insert_pages;
    uint64_t host_visible_offset_pages = 0;
    std::vector<std::string> planned_pages;
    std::vector<std::string> hit_pages;
    std::vector<std::string> completed_pages;
    HiCacheIoSchedule io_schedule;
    uint64_t completed_byte_count = 0;
    uint64_t policy_stop_ts = 0;
    uint64_t target_boundary_ts = 0;
    uint64_t timeout_deadline_ts = 0;
    bool payload_transfer_issued = false;
    bool timed_out = false;
    uint64_t requested_host_pages = 0;
    uint64_t reserved_host_pages = 0;
    bool visibility_dependency_required = false;
    HiCachePrefetchState prefetch_state = HiCachePrefetchState::Pending;
};

/** @brief Node-level writeback triggered by dirty L1 eviction. */
struct HiCacheWritebackOperation {
    HiCacheOperationHeader header;
};

/** @brief Modeled transfer of a host/storage-visible prefix back to L1. */
struct HiCacheLoadbackOperation {
    HiCacheOperationHeader header;
    HiCacheIoSchedule io_schedule;
};

/** @brief Modeled operation that commits an L2 value to storage. */
struct HiCacheStorageOperation {
    HiCacheOperationHeader header;
    std::vector<std::string> device_to_host_pages;
    std::vector<std::string> capacity_gate_pages;
};

/**
 * @brief Canonical table of target-derived asynchronous operations.
 *
 * Source-observed actual completion does not live here. Every stored state is derived for
 * the target configuration. The request reverse index intentionally contains prefetches
 * only because reservation drain is its sole production consumer.
 */
class HiCacheAsyncOperationTable {
public:
    /**
     * @brief Inserts a new target-derived prefetch operation.
     * @throws std::invalid_argument if its identity or kind is malformed.
     * @throws std::logic_error if the operation ID already exists.
     */
    void insert_prefetch(HiCachePrefetchOperation op);

    /** @brief Returns the latest prefetch operation for a request. */
    [[nodiscard]] HiCachePrefetchOperation * prefetch_for_request(const std::string & request_key);

    /** @brief Returns the latest prefetch operation for a request. */
    [[nodiscard]] const HiCachePrefetchOperation * prefetch_for_request(const std::string & request_key) const;

    /** @brief Advances prefetch-specific and common lifecycle state by operation ID. */
    void set_prefetch_state_by_id(const std::string & operation_id, HiCachePrefetchState prefetch_state, HiCacheOperationState operation_state,
                                  std::string_view reason, uint64_t transition_ts = 0);

    /** @brief Advances the latest prefetch operation for a request. */
    void set_prefetch_state(const std::string & request_key, HiCachePrefetchState prefetch_state, HiCacheOperationState operation_state,
                            std::string_view reason, uint64_t transition_ts = 0);

    /** @brief Returns the mutable prefetch index for finalization inside the state machine. */
    [[nodiscard]] std::unordered_map<std::string, HiCachePrefetchOperation> & prefetch_ops() { return prefetch_by_id_; }

    /** @brief Returns all prefetch operations. */
    [[nodiscard]] const std::unordered_map<std::string, HiCachePrefetchOperation> & prefetch_ops() const { return prefetch_by_id_; }

    /** @brief Counts pages still consuming prefetch request budget in a scope. */
    [[nodiscard]] uint64_t active_requested_pages(const std::string & cache_scope) const;

    /** @brief Counts pages still consuming L2 reservation budget in a scope. */
    [[nodiscard]] uint64_t reserved_pages(const std::string & cache_scope) const;

    /** @brief Releases drainable prefetch reservations at a request reuse/release boundary. */
    uint64_t release_prefetch_pending_host_pages_for_request(const std::string & request_key);

    /** @brief Inserts a new writeback operation and rejects duplicate IDs. */
    void insert_writeback(HiCacheWritebackOperation op);

    /** @brief Advances common lifecycle state for a writeback operation. */
    void set_writeback_state(const std::string & operation_id, HiCacheOperationState state, std::string_view reason, uint64_t transition_ts = 0);

    /** @brief Returns all writeback operations. */
    [[nodiscard]] const std::unordered_map<std::string, HiCacheWritebackOperation> & writeback_ops() const { return writeback_by_id_; }

    /** @brief Inserts a new modeled loadback operation and rejects duplicate IDs. */
    void insert_loadback(HiCacheLoadbackOperation op);

    /** @brief Returns the latest modeled loadback for a request. */
    [[nodiscard]] HiCacheLoadbackOperation * loadback_for_request(const std::string & request_key);

    /** @brief Returns the latest modeled loadback for a request. */
    [[nodiscard]] const HiCacheLoadbackOperation * loadback_for_request(const std::string & request_key) const;

    /** @brief Advances common lifecycle state for a loadback operation. */
    void set_loadback_state(const std::string & operation_id, HiCacheOperationState state, std::string_view reason, uint64_t transition_ts = 0);

    /** @brief Returns all loadback operations. */
    [[nodiscard]] const std::unordered_map<std::string, HiCacheLoadbackOperation> & loadback_ops() const { return loadback_by_id_; }

    /** @brief Inserts a new storage-backup operation and rejects duplicate IDs. */
    void insert_storage(HiCacheStorageOperation op);

    /** @brief Advances common lifecycle state for a storage operation. */
    void set_storage_state(const std::string & operation_id, HiCacheOperationState state, std::string_view reason, uint64_t transition_ts = 0);

    /** @brief Assigns pages protected by the write-through ACK/release gate. */
    void set_storage_capacity_gate_pages(const std::string & operation_id, std::vector<std::string> pages);

    /** @brief Records the first canonical consumer released by a storage capacity gate. */
    void set_storage_consumer_boundary(const std::string & operation_id, uint64_t consumer_epoch, uint64_t consumer_ts, size_t source_node_id,
                                       size_t source_event_index, std::string source_fact_role, bool source_available);

    /** @brief Returns all storage operations. */
    [[nodiscard]] const std::unordered_map<std::string, HiCacheStorageOperation> & storage_ops() const { return storage_by_id_; }

#ifdef DEBUG
    /** @brief Returns the structured lifecycle transition trace. */
    [[nodiscard]] const std::vector<HiCacheOperationLifecycleTransition> & lifecycle_transitions() const { return lifecycle_transitions_; }

    /** @brief Returns the lifecycle transition count. */
    [[nodiscard]] uint64_t lifecycle_transition_count() const { return lifecycle_epoch_; }
#endif

    /** @brief Returns a stable read-only view of prefetch IDs associated with a request. */
    [[nodiscard]] std::span<const std::string> operations_for_request(const std::string & request_key) const;

private:
    uint64_t lifecycle_epoch_ = 0;
    std::unordered_map<std::string, HiCachePrefetchOperation> prefetch_by_id_;
    std::unordered_map<std::string, std::string> latest_prefetch_id_by_request_;
    std::unordered_map<std::string, HiCacheWritebackOperation> writeback_by_id_;
    std::unordered_map<std::string, HiCacheLoadbackOperation> loadback_by_id_;
    std::unordered_map<std::string, std::string> latest_loadback_id_by_request_;
    std::unordered_map<std::string, HiCacheStorageOperation> storage_by_id_;
    std::unordered_map<std::string, std::vector<std::string>> operation_ids_by_request_;
#ifdef DEBUG
    std::vector<HiCacheOperationLifecycleTransition> lifecycle_transitions_;
#endif

    /** @brief Adds a prefetch ID to the request reservation-drain index. */
    void index_prefetch(const HiCacheOperationHeader & header);

    /** @brief Advances common lifecycle state and records a Debug transition. */
    void transition_header(HiCacheOperationHeader & header, HiCacheOperationState state, std::string_view reason, uint64_t transition_ts);
};

} // namespace markov::trace_graph::modules::hicache::runtime
