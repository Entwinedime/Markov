#pragma once

#include "trace_graph/modules/hicache/hicache_token_radix_tree.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace TraceGraph {

/**
 * @brief target-derived cache operation 的类型。
 *
 * SGLang 中 prefetch、writeback、loadback 和 storage backup 都有独立控制流，但
 * 它们共享 operation id、scope、request、node/page group 和 lifecycle 边界。
 */
enum class HiCacheOperationKind { Prefetch, Writeback, Loadback, Storage };

/**
 * @brief operation header 的统一生命周期状态。
 *
 * 当前模型仍然同步折叠 ack，但数据结构显式保留 created、queued、ready、
 * completed、committed 和 cancelled，避免物理语义退化成匿名 page mutation。
 */
enum class HiCacheOperationState { Created, Queued, Ready, Completed, Committed, Cancelled };

/**
 * @brief prefetch stop policy 产生的 modeled 结果。
 *
 * 该状态用于 final-state 派生和 prefetch 诊断；通用 operation lifecycle 保存在
 * `HiCacheOperationHeader::state`。
 */
enum class HiCachePrefetchState { Pending, Ready, Applied, Suppressed, Late, Revoked };

/**
 * @brief 所有 cache operation 共用的 target-derived header。
 *
 * header 只描述 operation 的身份、归属、作用 page/node 和生命周期时钟。prefetch
 * hit prefix、host reservation 等策略细节必须放在具体 operation 结构中。
 */
struct HiCacheOperationHeader {
    std::string operation_id;
    HiCacheOperationKind kind = HiCacheOperationKind::Prefetch;
    std::string cache_scope;
    std::string request_key;
    std::string owner;
    HiCacheNodeId anchor_node = 0;
    std::vector<HiCacheNodeId> node_ids;
    std::vector<std::string> pages;
    uint64_t byte_count = 0;
    HiCacheOperationState state = HiCacheOperationState::Created;
    uint64_t enqueue_epoch = 0;
    uint64_t eligible_epoch = 0;
    uint64_t checkpoint_epoch = 0;
    uint64_t complete_epoch = 0;
    uint64_t commit_epoch = 0;
    uint64_t enqueue_ts = 0;
    uint64_t checkpoint_ts = 0;
    uint64_t complete_ts = 0;
    uint64_t commit_ts = 0;
    uint64_t cancel_ts = 0;
    std::string cancel_reason;
};

/**
 * @brief operation lifecycle 变化的审计记录。
 *
 * 这是 async table 自己的结构化账本，不等同于 HiCache state transition；后者描述
 * residency/ref 如何变化，前者描述 operation 如何推进。
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

/**
 * @brief 单个 storage prefetch operation。
 *
 * planned_pages 是 page-aligned prefetch request；hit_pages 是 storage 连续命中前缀；
 * completed_pages 是 target I/O progress 在 terminate 边界已经传完的 prefix；
 * reserved_host_pages 是 host allocation/cleanup 后实际 reservation。
 */
struct HiCachePrefetchOperation {
    HiCacheOperationHeader header;
    std::vector<HiCacheNodeId> anchor_chain;
    std::vector<std::string> host_insert_pages;
    uint64_t host_visible_offset_pages = 0;
    std::vector<std::string> planned_pages;
    std::vector<std::string> hit_pages;
    std::vector<std::string> completed_pages;
    uint64_t requested_host_pages = 0;
    uint64_t reserved_host_pages = 0;
    int64_t priority = 0;
    HiCachePrefetchState prefetch_state = HiCachePrefetchState::Pending;
};

/** @brief write-back eviction 触发的 node-level writeback operation。 */
struct HiCacheWritebackOperation {
    HiCacheOperationHeader header;
    HiCacheNodeId node_id = 0;
};

/** @brief host/storage 可见 prefix 重新进入 device 的 modeled loadback operation。 */
struct HiCacheLoadbackOperation {
    HiCacheOperationHeader header;
    HiCacheNodeId target_node = 0;
};

/** @brief host value 写入 storage backend 的 modeled storage operation。 */
struct HiCacheStorageOperation {
    HiCacheOperationHeader header;
    HiCacheNodeId node_id = 0;
};

/**
 * @brief modeled async operation table。
 *
 * 这里不保存 source actual completion，只保存 target-derived operation 状态。
 */
class HiCacheAsyncOperationTable {
public:
    void upsert_prefetch(HiCachePrefetchOperation op);
    [[nodiscard]] HiCachePrefetchOperation * prefetch_for_request(const std::string & request_key);
    [[nodiscard]] const HiCachePrefetchOperation * prefetch_for_request(const std::string & request_key) const;
    void set_prefetch_state_by_id(const std::string & operation_id, HiCachePrefetchState prefetch_state, HiCacheOperationState operation_state,
                                  const std::string & reason, uint64_t transition_ts = 0);
    void set_prefetch_state(const std::string & request_key, HiCachePrefetchState prefetch_state, HiCacheOperationState operation_state,
                            const std::string & reason, uint64_t transition_ts = 0);

    [[nodiscard]] std::unordered_map<std::string, HiCachePrefetchOperation> & prefetch_ops() { return prefetch_by_id_; }
    [[nodiscard]] const std::unordered_map<std::string, HiCachePrefetchOperation> & prefetch_ops() const { return prefetch_by_id_; }
    [[nodiscard]] uint64_t active_requested_pages(const std::string & cache_scope) const;
    [[nodiscard]] uint64_t reserved_pages(const std::string & cache_scope) const;
    uint64_t release_deferred_host_pages(const std::string & cache_scope);

    void upsert_writeback(HiCacheWritebackOperation op);
    void set_writeback_state(const std::string & operation_id, HiCacheOperationState state, const std::string & reason, uint64_t transition_ts = 0);
    [[nodiscard]] const std::unordered_map<std::string, HiCacheWritebackOperation> & writeback_ops() const { return writeback_by_id_; }

    void upsert_loadback(HiCacheLoadbackOperation op);
    void set_loadback_state(const std::string & operation_id, HiCacheOperationState state, const std::string & reason, uint64_t transition_ts = 0);
    [[nodiscard]] const std::unordered_map<std::string, HiCacheLoadbackOperation> & loadback_ops() const { return loadback_by_id_; }

    void upsert_storage(HiCacheStorageOperation op);
    void set_storage_state(const std::string & operation_id, HiCacheOperationState state, const std::string & reason, uint64_t transition_ts = 0);
    [[nodiscard]] const std::unordered_map<std::string, HiCacheStorageOperation> & storage_ops() const { return storage_by_id_; }
    [[nodiscard]] const std::vector<HiCacheOperationLifecycleTransition> & lifecycle_transitions() const { return lifecycle_transitions_; }
    [[nodiscard]] uint64_t lifecycle_transition_count() const { return static_cast<uint64_t>(lifecycle_transitions_.size()); }
    [[nodiscard]] std::vector<std::string> operations_for_request(const std::string & request_key) const;
    [[nodiscard]] std::vector<std::string> operations_for_node(HiCacheNodeId node_id) const;

private:
    uint64_t lifecycle_epoch_ = 0;
    std::unordered_map<std::string, HiCachePrefetchOperation> prefetch_by_id_;
    std::unordered_map<std::string, std::string> latest_prefetch_id_by_request_;
    std::unordered_map<std::string, HiCacheWritebackOperation> writeback_by_id_;
    std::unordered_map<std::string, HiCacheLoadbackOperation> loadback_by_id_;
    std::unordered_map<std::string, HiCacheStorageOperation> storage_by_id_;
    std::unordered_map<std::string, std::vector<std::string>> operation_ids_by_request_;
    std::unordered_map<HiCacheNodeId, std::vector<std::string>> operation_ids_by_node_;
    std::vector<HiCacheOperationLifecycleTransition> lifecycle_transitions_;

    void index_operation(const HiCacheOperationHeader & header);
    void transition_header(HiCacheOperationHeader & header, HiCacheOperationState state, const std::string & reason, uint64_t transition_ts);
};

} // namespace TraceGraph
