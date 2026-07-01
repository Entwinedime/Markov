/**
 * @file
 * @brief HiCache lock/host ref owner ledger 和 tree ref 审计。
 */
#pragma once

#include "markov/trace_graph/modules/hicache/radix/token_radix_tree.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

using radix::HiCacheNodeId;
using radix::HiCacheTokenRadixTree;

/**
 * @brief 单个 ref owner 当前持有的 node-level 引用。
 *
 * tree 上的 ref counter 是最终状态源；ledger 保存 owner 到 node chain 的审计账，
 * 用于保证 request、write、load、storage 和 prefetch 可以独立释放。
 */
struct HiCacheRefOwnerRecord {
    std::string owner_id;
    std::string owner_kind;
    std::string request_key;
    std::string operation_id;
    std::vector<HiCacheNodeId> lock_nodes;
    std::vector<HiCacheNodeId> host_nodes;
    uint64_t acquire_epoch = 0;
    uint64_t release_epoch = 0;
    bool active = false;
};

/**
 * @brief 一次 ref acquire/release 的可审计结果。
 */
struct HiCacheRefMutation {
    uint64_t mutation_epoch = 0;
    std::string cache_scope;
    std::string action;
    std::string owner_id;
    std::string owner_kind;
    std::string request_key;
    std::string operation_id;
    std::string reason;
    std::vector<HiCacheNodeId> lock_nodes;
    std::vector<HiCacheNodeId> host_nodes;
    std::vector<std::string> lock_pages;
    std::vector<std::string> host_pages;
    int64_t lock_ref_delta = 0;
    int64_t host_ref_delta = 0;
    bool changed = false;
};

/**
 * @brief ref ledger 与 canonical tree ref counter 的一致性问题。
 */
struct HiCacheRefAuditIssue {
    std::string cache_scope;
    std::string issue;
    std::string ref_kind;
    std::string owner_id;
    HiCacheNodeId node_id = 0;
    uint64_t ledger_count = 0;
    uint64_t tree_count = 0;
    std::vector<std::string> pages;
};

/**
 * @brief ref ledger 的一致性审计结果。
 */
struct HiCacheRefAudit {
    uint64_t active_owner_count = 0;
    uint64_t ledger_lock_ref_count = 0;
    uint64_t ledger_host_ref_count = 0;
    uint64_t tree_lock_ref_count = 0;
    uint64_t tree_host_ref_count = 0;
    std::vector<HiCacheRefAuditIssue> issues;

    /** @brief ref ledger 与 canonical tree ref counter 是否完全一致。 */
    [[nodiscard]] bool ok() const { return issues.empty(); }
};

/**
 * @brief HiCache node-level ref 的 owner 账本。
 *
 * SGLang 的 lock_ref/host_lock_ref 挂在 radix node 上；本账本只负责把不同
 * lifecycle owner 的 acquire/release 收敛到统一入口，不成为另一个 page set 状态源。
 */
class HiCacheRefLedger {
public:
    /** @brief 释放某个 owner 当前持有的所有 lock/host 引用。 */
    HiCacheRefMutation release_owner(HiCacheTokenRadixTree & tree, const std::string & owner_id);

    /** @brief 为 owner 持有一条 ordinary lock ref node chain。 */
    HiCacheRefMutation acquire_lock(HiCacheTokenRadixTree & tree, const std::string & owner_id, const std::string & owner_kind, const std::string & request_key,
                                    const std::string & operation_id, const std::vector<HiCacheNodeId> & nodes);

    /** @brief 为 owner 持有一条 host ref node chain。 */
    HiCacheRefMutation acquire_host(HiCacheTokenRadixTree & tree, const std::string & owner_id, const std::string & owner_kind, const std::string & request_key,
                                    const std::string & operation_id, const std::vector<HiCacheNodeId> & nodes);

    /** @brief 将 radix split 复制出来的 tree ref 同步回 owner 账本。 */
    void sync_tree_ref_copies(const HiCacheTokenRadixTree & tree, const std::string & reason);

    /** @brief 查询 owner 记录；不存在时返回空指针。 */
    [[nodiscard]] const HiCacheRefOwnerRecord * owner(const std::string & owner_id) const;

    /** @brief 所有 owner 记录。 */
    [[nodiscard]] const std::unordered_map<std::string, HiCacheRefOwnerRecord> & owners() const { return owners_; }

#ifdef DEBUG
    /** @brief ref lifecycle mutation 的审计 trace。 */
    [[nodiscard]] const std::vector<HiCacheRefMutation> & mutation_trace() const { return mutation_trace_; }

    /** @brief 审计 ledger 与 tree ref counter 是否一致。 */
    [[nodiscard]] HiCacheRefAudit audit(const HiCacheTokenRadixTree & tree) const;
#endif

    /** @brief 当前 active owner 数。 */
    [[nodiscard]] uint64_t active_owner_count() const;

    /** @brief 已记录的 ref mutation 次数。 */
    [[nodiscard]] uint64_t mutation_count() const { return epoch_; }

private:
    uint64_t epoch_ = 0;
    std::unordered_map<std::string, HiCacheRefOwnerRecord> owners_;
#ifdef DEBUG
    std::vector<HiCacheRefMutation> mutation_trace_;
#endif

    [[nodiscard]] HiCacheRefOwnerRecord & ensure_owner(const std::string & owner_id, const std::string & owner_kind, const std::string & request_key,
                                                       const std::string & operation_id);
    [[nodiscard]] static std::vector<std::string> flatten_pages(const HiCacheTokenRadixTree & tree, const std::vector<HiCacheNodeId> & nodes);
    void record_mutation(HiCacheRefMutation mutation);
};

} // namespace markov::trace_graph::modules::hicache::runtime
