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


private:
    /** @brief Non-owning diagnostic identity supplied while acquiring an owner record. */
    struct OwnerMetadata {
        const std::string & owner_kind;
        const std::string & request_key;
        const std::string & operation_id;
    };

    std::unordered_map<std::string, HiCacheRefOwnerRecord> owners_;

    [[nodiscard]] HiCacheRefOwnerRecord & ensure_owner(const std::string & owner_id, const OwnerMetadata & metadata);
};

} // namespace markov::trace_graph::modules::hicache::runtime
