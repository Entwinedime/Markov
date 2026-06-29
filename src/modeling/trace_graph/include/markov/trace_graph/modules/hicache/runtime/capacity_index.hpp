/**
 * @file
 * @brief HiCache host/device capacity 投影索引和审计记录。
 */
#pragma once

#include "markov/trace_graph/modules/hicache/radix/token_radix_tree.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

using radix::HiCacheCacheNode;
using radix::HiCacheNodeId;
using radix::HiCacheNodeRefState;
using radix::HiCacheNodeResidency;
using radix::HiCacheTokenRadixTree;

/**
 * @brief 单个 node 在 capacity index 中的可审计投影。
 *
 * record 不成为 residency 状态源，只缓存 leaf eligibility 和排序字段，避免 capacity
 * handler 反复扫描 canonical tree。
 */
struct HiCacheCapacityNodeRecord {
    HiCacheNodeId node_id = 0;
    HiCacheNodeId parent = 0;
    bool active = false;
    uint64_t page_count = 0;
    std::vector<std::string> pages;
    int64_t priority = 0;
    uint64_t last_access_order = 0;
    HiCacheNodeResidency residency;
    HiCacheNodeRefState refs;
    bool device_evictable = false;
    bool host_evictable = false;
    uint64_t observed_epoch = 0;
};

/**
 * @brief 一次 capacity index 同步的审计记录。
 */
struct HiCacheCapacityMutation {
    uint64_t mutation_epoch = 0;
    std::string cache_scope;
    std::string reason;
    uint64_t reserved_host_pages = 0;
    std::vector<HiCacheNodeId> observed_nodes;
    std::vector<HiCacheNodeId> device_leaf_entered;
    std::vector<HiCacheNodeId> device_leaf_left;
    std::vector<HiCacheNodeId> host_leaf_entered;
    std::vector<HiCacheNodeId> host_leaf_left;
};

/**
 * @brief 一次 capacity victim 选择的解释记录。
 */
struct HiCacheCapacityVictimChoice {
    uint64_t selection_epoch = 0;
    std::string cache_scope;
    std::string tier;
    std::string reason;
    bool selected = false;
    HiCacheNodeId node_id = 0;
    uint64_t page_count = 0;
    int64_t priority = 0;
    uint64_t last_access_order = 0;
    uint64_t occupied_pages = 0;
    uint64_t reserved_host_pages = 0;
    uint64_t capacity_pages = 0;
    uint64_t requested_pages = 0;
    uint64_t excess_pages = 0;
    std::vector<std::string> pages;
};

/**
 * @brief capacity index 与 canonical tree 的一致性问题。
 */
struct HiCacheCapacityAuditIssue {
    std::string cache_scope;
    std::string issue;
    std::string tier;
    HiCacheNodeId node_id = 0;
    uint64_t indexed_count = 0;
    uint64_t tree_count = 0;
    std::vector<std::string> pages;
};

/**
 * @brief capacity index 一致性审计结果。
 */
struct HiCacheCapacityAudit {
    uint64_t indexed_device_pages = 0;
    uint64_t indexed_host_pages = 0;
    uint64_t indexed_storage_pages = 0;
    uint64_t indexed_reserved_host_pages = 0;
    uint64_t tree_device_pages = 0;
    uint64_t tree_host_pages = 0;
    uint64_t tree_storage_pages = 0;
    uint64_t expected_reserved_host_pages = 0;
    std::vector<HiCacheCapacityAuditIssue> issues;

    /** @brief capacity index 与 canonical tree 是否完全一致。 */
    [[nodiscard]] bool ok() const { return issues.empty(); }
};

/**
 * @brief 单个 cache_scope 的容量索引快照。
 */
struct HiCacheCapacitySnapshot {
    uint64_t occupied_device_pages = 0;
    uint64_t occupied_host_pages = 0;
    uint64_t readable_storage_pages = 0;
    uint64_t reserved_host_pages = 0;
    std::vector<HiCacheNodeId> evictable_device_leaves;
    std::vector<HiCacheNodeId> evictable_host_leaves;
};

/**
 * @brief HiCache L1/L2 capacity 的显式 leaf index。
 *
 * SGLang 在 insert/split/evict/ref 变化时更新 evictable leaf 集合。该索引同样由
 * tree/ref/reservation mutation 显式同步，不再让 capacity handler 自己扫描 tree。
 */
class HiCacheCapacityIndex {
public:
    /** @brief 同步一组发生变化的 node、其祖先和必要子节点。 */
    HiCacheCapacityMutation sync_nodes(const HiCacheTokenRadixTree & tree, const std::vector<HiCacheNodeId> & seed_nodes, uint64_t reserved_host_pages,
                                       const std::string & reason);

    /** @brief 只同步 async reservation，不观察 tree node。 */
    HiCacheCapacityMutation sync_reservation(uint64_t reserved_host_pages, const std::string & reason);

    /** @brief 当前容量快照。 */
    [[nodiscard]] const HiCacheCapacitySnapshot & snapshot() const { return snapshot_; }

    /** @brief 当前已缓存的 node record。 */
    [[nodiscard]] const std::map<HiCacheNodeId, HiCacheCapacityNodeRecord> & node_records() const { return records_; }

    /** @brief 已发生的 capacity index mutation 次数。 */
    [[nodiscard]] uint64_t mutation_epoch() const { return mutation_epoch_; }

    /** @brief capacity leaf mutation 的审计 trace。 */
    [[nodiscard]] const std::vector<HiCacheCapacityMutation> & mutation_trace() const { return mutation_trace_; }

    /** @brief victim 选择解释 trace。 */
    [[nodiscard]] const std::vector<HiCacheCapacityVictimChoice> & victim_choices() const { return victim_choices_; }

    /** @brief 审计 index 与 canonical tree 是否一致。 */
    [[nodiscard]] HiCacheCapacityAudit audit(const HiCacheTokenRadixTree & tree, uint64_t expected_reserved_host_pages) const;

    /** @brief device 层超过 target capacity 的 page 数。 */
    [[nodiscard]] uint64_t device_excess_pages(uint64_t capacity_pages) const;

    /** @brief host 层超过 target capacity 的 page 数，包含 prefetch reservation。 */
    [[nodiscard]] uint64_t host_excess_pages(uint64_t capacity_pages) const;

    /** @brief 当前 device victim；无可驱逐 leaf 时为空。 */
    [[nodiscard]] std::optional<HiCacheNodeId> first_device_victim() const;

    /** @brief 当前 host victim；无可清理 leaf 时为空。 */
    [[nodiscard]] std::optional<HiCacheNodeId> first_host_victim() const;

    /** @brief 记录并返回当前 device victim。 */
    std::optional<HiCacheNodeId> select_device_victim(uint64_t capacity_pages, uint64_t requested_pages, const std::string & reason);

    /** @brief 记录并返回当前 host victim。 */
    std::optional<HiCacheNodeId> select_host_victim(uint64_t capacity_pages, uint64_t requested_pages, const std::string & reason);

private:
    struct VictimKey {
        int64_t priority = 0;
        uint64_t last_access_order = 0;
        HiCacheNodeId node_id = 0;

        [[nodiscard]] bool operator<(const VictimKey & other) const;
    };

    uint64_t mutation_epoch_ = 0;
    uint64_t occupied_device_pages_ = 0;
    uint64_t occupied_host_pages_ = 0;
    uint64_t readable_storage_pages_ = 0;
    uint64_t reserved_host_pages_ = 0;
    uint64_t victim_selection_epoch_ = 0;
    std::map<HiCacheNodeId, HiCacheCapacityNodeRecord> records_;
    std::set<VictimKey> evictable_device_leaves_;
    std::set<VictimKey> evictable_host_leaves_;
    HiCacheCapacitySnapshot snapshot_;
    std::vector<HiCacheCapacityMutation> mutation_trace_;
    std::vector<HiCacheCapacityVictimChoice> victim_choices_;

    [[nodiscard]] HiCacheCapacityNodeRecord make_record(const HiCacheTokenRadixTree & tree, HiCacheNodeId node_id) const;
    [[nodiscard]] std::set<HiCacheNodeId> observation_closure(const HiCacheTokenRadixTree & tree, const std::vector<HiCacheNodeId> & seed_nodes) const;
    [[nodiscard]] bool has_device_descendant(const HiCacheTokenRadixTree & tree, const HiCacheCacheNode & node) const;
    [[nodiscard]] bool has_backup_child(const HiCacheTokenRadixTree & tree, const HiCacheCacheNode & node) const;
    void remove_record_contribution(const HiCacheCapacityNodeRecord & record);
    void add_record_contribution(const HiCacheCapacityNodeRecord & record);
    void record_mutation(HiCacheCapacityMutation mutation);
    [[nodiscard]] std::optional<HiCacheCapacityNodeRecord> indexed_record(HiCacheNodeId node_id) const;
    [[nodiscard]] HiCacheCapacityVictimChoice make_victim_choice(const std::string & tier, const std::string & reason, uint64_t capacity_pages,
                                                                 uint64_t requested_pages, std::optional<HiCacheNodeId> node_id) const;
    [[nodiscard]] std::map<HiCacheNodeId, HiCacheCapacityNodeRecord> derive_tree_records(const HiCacheTokenRadixTree & tree) const;
    void update_snapshot();
    [[nodiscard]] static VictimKey victim_key(const HiCacheCapacityNodeRecord & record);
};

} // namespace markov::trace_graph::modules::hicache::runtime
