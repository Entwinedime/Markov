/**
 * @file
 * @brief HiCache lock/host reference ownership ledger and Debug tree audit.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/radix/token_radix_tree.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

using radix::HiCacheNodeId;
using radix::HiCacheTokenRadixTree;

/**
 * @brief Node-level references currently held by one lifecycle owner.
 *
 * Radix-node counters are the canonical reference state. The ledger supplies the reverse
 * owner-to-node mapping required to release request, write, load, storage, and prefetch
 * lifecycles independently. Descriptive owner metadata is Debug-only because policy reads
 * only the node chains and active flag.
 */
struct HiCacheRefOwnerRecord {
    std::vector<HiCacheNodeId> lock_nodes;
    std::vector<HiCacheNodeId> host_nodes;
    bool active = false;
#ifdef DEBUG
    std::string owner_kind;
    std::string request_key;
    std::string operation_id;
    uint64_t acquire_epoch = 0;
    uint64_t release_epoch = 0;
#endif
};

/**
 * @brief Minimal production result identifying nodes affected by a reference mutation.
 *
 * Capacity synchronization needs only this set. Keeping the diagnostic mutation separate
 * prevents Release builds from flattening page paths and copying owner metadata for a
 * value that callers immediately discard.
 */
struct HiCacheRefChange {
    std::vector<HiCacheNodeId> affected_nodes;
};

#ifdef DEBUG
/**
 * @brief Detailed diagnostic record for one reference acquire, release, or split copy.
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
 * @brief One consistency discrepancy between the ledger and radix-node counters.
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
 * @brief Full Debug audit result for the reference ledger.
 */
struct HiCacheRefAudit {
    uint64_t active_owner_count = 0;
    uint64_t ledger_lock_ref_count = 0;
    uint64_t ledger_host_ref_count = 0;
    uint64_t tree_lock_ref_count = 0;
    uint64_t tree_host_ref_count = 0;
    std::vector<HiCacheRefAuditIssue> issues;

    /** @brief Returns true when owner chains exactly match radix-node counters. */
    [[nodiscard]] bool ok() const { return issues.empty(); }
};
#endif

/**
 * @brief Owner ledger for HiCache node-level lock and host references.
 *
 * SGLang stores lock and host-reference counters on radix nodes. This ledger centralizes
 * lifecycle acquire/release operations but does not become another residency or page-set
 * source of truth. Split synchronization mirrors reference copies made by the radix tree
 * so a later owner release can still remove every copied counter.
 */
class HiCacheRefLedger {
public:
    /** @brief Releases every lock and host reference held by an owner. */
    HiCacheRefChange release_owner(HiCacheTokenRadixTree & tree, const std::string & owner_id);

    /** @brief Acquires an ordinary lock-reference node chain for an owner. */
    HiCacheRefChange acquire_lock(HiCacheTokenRadixTree & tree, const std::string & owner_id, const std::string & owner_kind, const std::string & request_key,
                                  const std::string & operation_id, const std::vector<HiCacheNodeId> & nodes);

    /** @brief Acquires a host-reference node chain for an owner. */
    HiCacheRefChange acquire_host(HiCacheTokenRadixTree & tree, const std::string & owner_id, const std::string & owner_kind, const std::string & request_key,
                                  const std::string & operation_id, const std::vector<HiCacheNodeId> & nodes);

    /** @brief Mirrors references copied by radix splits into the owner reverse index. */
    void sync_tree_ref_copies(const HiCacheTokenRadixTree & tree, std::string_view reason);

#ifdef DEBUG
    /** @brief Returns the detailed reference lifecycle trace. */
    [[nodiscard]] const std::vector<HiCacheRefMutation> & mutation_trace() const { return mutation_trace_; }

    /** @brief Independently checks owner chains against canonical radix-node counters. */
    [[nodiscard]] HiCacheRefAudit audit(const HiCacheTokenRadixTree & tree) const;

    /** @brief Returns the number of owners currently holding any reference. */
    [[nodiscard]] uint64_t active_owner_count() const;

    /** @brief Returns the number of recorded reference mutations. */
    [[nodiscard]] uint64_t mutation_count() const { return epoch_; }
#endif

private:
    /** @brief Non-owning diagnostic identity supplied while acquiring an owner record. */
    struct OwnerMetadata {
        const std::string & owner_kind;
        const std::string & request_key;
        const std::string & operation_id;
    };

    std::unordered_map<std::string, HiCacheRefOwnerRecord> owners_;
#ifdef DEBUG
    uint64_t epoch_ = 0;
    std::vector<HiCacheRefMutation> mutation_trace_;
#endif

    [[nodiscard]] HiCacheRefOwnerRecord & ensure_owner(const std::string & owner_id, const OwnerMetadata & metadata);
#ifdef DEBUG
    [[nodiscard]] static std::vector<std::string> flatten_pages(const HiCacheTokenRadixTree & tree, const std::vector<HiCacheNodeId> & nodes);
#endif
};

} // namespace markov::trace_graph::modules::hicache::runtime
