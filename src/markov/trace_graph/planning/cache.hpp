#pragma once

#include "markov/trace_graph/planning/radix_tree.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace markov::trace_graph::planning {

enum class Tier { Device, Host };
using BufferId = std::size_t;
using TransferId = std::size_t;

struct CacheConfig {
    // Page capacities are per participant; the controller reports publication
    // only after all participants required by a consumer have reached it.
    std::size_t device_pages;
    std::size_t host_pages;
    std::size_t layers;
    // The framework supplies its hit threshold; nullopt means dirty data is
    // backed up on eviction instead of being backed up by insertion hits.
    std::optional<std::size_t> backup_after_hits;
};

struct CacheSlot {
    // No producer means fully published. In-flight progress belongs only to
    // its transfer, including when a radix split divides the destination.
    std::optional<TransferId> producer = std::nullopt;
};

struct CacheNode {
    std::optional<CacheSlot> device;
    std::optional<CacheSlot> host;
    std::size_t device_refs = 0;
    std::size_t host_refs = 0;
    std::size_t hits = 0;
    std::size_t access_sequence = 0;
};

struct CacheMatch {
    RadixId node;
    RadixId device;
    RadixId host;
    std::size_t pages;
};

enum class CopyKind { Backup, Load };
enum class CopyStatus { Started, AlreadyPresent, SourceNotReady, InsufficientCapacity };

struct CopyStart {
    CopyStatus status;
    std::optional<TransferId> transfer;
    std::size_t pages;
};

enum class Eviction { Removed, Blocked, BackupRequired };

// One target owns one cache. This class manages capacity and data visibility;
// it neither estimates costs nor advances time. Publication and acknowledgement
// are distinct calls from the shared execution clock.
class Cache {
public:
    explicit Cache(CacheConfig config) : config_(config) {}

    CacheMatch match(std::span<const PrefixId> pages);
    const RadixTree & tree() const { return tree_; }
    const CacheNode & node(RadixId id) const { return nodes_[id]; }
    std::size_t page_count(RadixId node) const;
    std::size_t used_pages(Tier tier) const;
    std::size_t peak_pages(Tier tier) const { return tier == Tier::Device ? peak_device_pages_ : peak_host_pages_; }
    std::size_t available_pages(Tier tier) const;
    std::size_t ready_layers(RadixId node, Tier tier) const;

    // A buffer remains allocated until its owner commits pages or releases it.
    // Prefetch callers commit only the accepted prefix and retain any in-flight
    // remainder until the worker has actually returned.
    std::optional<BufferId> reserve(Tier tier, std::size_t pages);
    RadixId commit_device(std::span<const PrefixId> pages, BufferId buffer);
    RadixId commit_host(std::span<const PrefixId> pages, BufferId buffer);
    void release(BufferId buffer);
    void trim(BufferId buffer, std::size_t remaining_pages);
    std::size_t reclaim_host(std::size_t required_pages);

    void pin_device(RadixId node);
    void unpin_device(RadixId node);
    void pin_host(RadixId node);
    void unpin_host(RadixId node);
    bool backup_required(RadixId node) const;
    CopyStart begin_backup(RadixId node);
    CopyStart begin_load(RadixId node);
    // Load layers arrive in order; acknowledgement follows full publication.
    void publish_layer(TransferId transfer);
    void publish_all(TransferId transfer);
    void acknowledge(TransferId transfer);

    // Whole-segment LRU order, with parents becoming candidates only after
    // their resident children. Pinned or in-flight segments stay allocated.
    std::vector<RadixId> eviction_order(Tier tier) const;
    Eviction evict_device(RadixId node);
    bool evict_host(RadixId node);

private:
    struct Buffer {
        Tier tier;
        std::size_t pages;
    };
    struct Copy {
        CopyKind kind;
        RadixId source_pin;
        std::vector<RadixId> nodes;
        std::size_t published_layers = 0;
    };

    std::optional<CacheSlot> & slot(RadixId node, Tier tier);
    const std::optional<CacheSlot> & slot(RadixId node, Tier tier) const;
    std::vector<RadixId> path(RadixId node) const;
    void apply_split(const RadixMatch & change);
    void touch(const RadixMatch & change);
    bool take_pages(Tier tier, std::size_t pages);
    void return_pages(Tier tier, std::size_t pages);
    void detach_empty_leaf(RadixId node);

    CacheConfig config_;
    RadixTree tree_;
    // The empty root is published in both tiers without allocating any pages.
    std::vector<CacheNode> nodes_{
        CacheNode{ CacheSlot{}, CacheSlot{} }
    };
    std::size_t used_device_pages_ = 0;
    std::size_t used_host_pages_ = 0;
    std::size_t peak_device_pages_ = 0, peak_host_pages_ = 0;
    std::size_t next_identity_ = 0;
    std::size_t access_sequence_ = 0;
    std::map<BufferId, Buffer> buffers_;
    std::map<TransferId, Copy> copies_;
};

} // namespace markov::trace_graph::planning
