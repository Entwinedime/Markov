#include "markov/trace_graph/planning/cache.hpp"

#include <algorithm>
#include <functional>
#include <queue>
#include <stdexcept>
#include <utility>

namespace markov::trace_graph::planning {

std::optional<CacheSlot> & Cache::slot(RadixId node, Tier tier) { return tier == Tier::Device ? nodes_[node].device : nodes_[node].host; }

const std::optional<CacheSlot> & Cache::slot(RadixId node, Tier tier) const { return tier == Tier::Device ? nodes_[node].device : nodes_[node].host; }

std::vector<RadixId> Cache::path(RadixId node) const {
    std::vector<RadixId> result;
    while (node != 0) {
        result.push_back(node);
        node = tree_.nodes()[node].parent;
    }
    std::ranges::reverse(result);
    return result;
}

void Cache::apply_split(const RadixMatch & change, std::vector<IndexCopy> & copies) {
    nodes_.resize(tree_.nodes().size());
    if (!change.split_child) return;

    const auto child = *change.split_child;
    const auto prefix = tree_.nodes()[child].parent;
    nodes_[prefix] = nodes_[child];
    nodes_[prefix].host_refs = 0;
    for (const auto tier : { Tier::Device, Tier::Host }) {
        const auto & allocation = slot(prefix, tier);
        if (allocation) {
            copies.push_back({ tier, tree_.nodes()[prefix].pages.size() });
            copies.push_back({ tier, tree_.nodes()[child].pages.size() });
        }
        if (allocation && allocation->producer) publications_.at(*allocation->producer).nodes.push_back(prefix);
    }
}

void Cache::touch(const RadixMatch & change) {
    const auto visited = path(change.node);
    for (const auto id : visited) {
        if (id == change.node && change.split_child) nodes_[*change.split_child].access_sequence = ++access_sequence_;
        nodes_[id].access_sequence = ++access_sequence_;
    }
}

CacheMatch Cache::match(std::span<const PrefixId> pages) {
    const auto result = tree_.match(pages);
    std::vector<IndexCopy> copies;
    apply_split(result, copies);
    touch(result);

    auto device = result.node, host = result.node;
    while (device != 0 && !nodes_[device].device) device = tree_.nodes()[device].parent;
    while (host != 0 && !nodes_[host].host) host = tree_.nodes()[host].parent;
    return { result.node, device, host, result.pages, std::move(copies) };
}

std::size_t Cache::page_count(RadixId node) const {
    std::size_t result = 0;
    while (node != 0) {
        result += tree_.nodes()[node].pages.size();
        node = tree_.nodes()[node].parent;
    }
    return result;
}

std::size_t Cache::used_pages(Tier tier) const { return tier == Tier::Device ? used_device_pages_ : used_host_pages_; }

std::size_t Cache::available_pages(Tier tier) const {
    return tier == Tier::Device ? free_device_pages() - reserved_device_pages_ : config_.host_pages - used_host_pages_;
}

std::size_t Cache::ready_layers(RadixId node, Tier tier) const {
    const auto & allocation = slot(node, tier);
    if (!allocation) return 0;
    return allocation->producer ? publications_.at(*allocation->producer).published_layers : config_.layers;
}

std::size_t Cache::prefix_ready_layers(RadixId node, Tier tier) const {
    auto layers = config_.layers;
    while (node != 0) {
        layers = std::min(layers, ready_layers(node, tier));
        node = tree_.nodes()[node].parent;
    }

    return layers;
}

bool Cache::take_pages(Tier tier, std::size_t pages) {
    if (pages > available_pages(tier)) return false;
    occupy_pages(tier, pages);
    return true;
}

void Cache::occupy_pages(Tier tier, std::size_t pages) {
    (tier == Tier::Device ? used_device_pages_ : used_host_pages_) += pages;
    if (tier == Tier::Device) device_allocation_run_ += pages;
    auto & peak = tier == Tier::Device ? peak_device_pages_ : peak_host_pages_;
    peak = std::max(peak, used_pages(tier));
}

void Cache::return_pages(Tier tier, std::size_t pages) {
    (tier == Tier::Device ? used_device_pages_ : used_host_pages_) -= pages;
    if (tier == Tier::Device && pages) device_allocation_run_ = 0;
}

std::optional<BufferId> Cache::reserve(Tier tier, std::size_t pages) {
    if (!take_pages(tier, pages)) return std::nullopt;

    const auto id = next_identity_++;
    buffers_.emplace(id, Buffer{ tier, pages });
    return id;
}

std::optional<BufferId> Cache::reserve_request(std::size_t pages) {
    if (pages > available_pages(Tier::Device)) return std::nullopt;
    const auto id = next_identity_++;
    reserved_device_pages_ += pages;
    buffers_.emplace(id, Buffer{ Tier::Device, 0, pages });
    return id;
}

void Cache::allocate_reserved(BufferId buffer, std::size_t pages) {
    auto & allocation = buffers_.at(buffer);
    if (allocation.tier != Tier::Device || pages > allocation.unallocated_pages)
        throw std::runtime_error("Native allocation exceeds its admitted request reservation");
    allocation.unallocated_pages -= pages;
    reserved_device_pages_ -= pages;
    allocation.pages += pages;
    occupy_pages(Tier::Device, pages);
}

PublicationId Cache::begin_device_publication() {
    const auto id = next_identity_++;
    publications_.emplace(id, Publication{ Tier::Device, {}, 0, std::nullopt });
    return id;
}

std::size_t Cache::publication_pages(PublicationId publication) const {
    std::size_t pages = 0;
    for (const auto node : publications_.at(publication).nodes) pages += tree_.nodes()[node].pages.size();
    return pages;
}

DeviceCommit Cache::commit_device(std::span<const PrefixId> pages, BufferId buffer, std::optional<PublicationId> producer) {
    auto matched = match(pages);
    auto & allocation = buffers_.at(buffer);
    std::size_t needed = pages.size() - matched.pages;
    for (const auto id : path(matched.node)) {
        if (!nodes_[id].device) needed += tree_.nodes()[id].pages.size();
    }
    if (allocation.tier != Tier::Device || allocation.pages < needed) throw std::runtime_error("Device commit exceeds its reserved buffer");

    const auto inserted = tree_.insert(pages);
    apply_split(inserted, matched.index_copies);
    for (const auto id : path(inserted.node)) {
        const bool present = nodes_[id].device.has_value();
        if (!present) {
            matched.index_copies.push_back({ Tier::Device, tree_.nodes()[id].pages.size() });
            nodes_[id].device = CacheSlot{ producer };
            if (producer) publications_.at(*producer).nodes.push_back(id);
            allocation.pages -= tree_.nodes()[id].pages.size();
        }
        const bool new_suffix = id == inserted.node && inserted.pages < pages.size();
        if (config_.backup_after_hits && (present || new_suffix)) ++nodes_[id].hits;
    }
    touch(inserted);
    return { inserted.node, std::move(matched.index_copies), needed };
}

RadixId Cache::commit_host(std::span<const PrefixId> pages, BufferId buffer) {
    const auto matched = match(pages);
    auto & allocation = buffers_.at(buffer);
    const auto needed = pages.size() - matched.pages;
    if (allocation.tier != Tier::Host || allocation.pages < needed) throw std::runtime_error("Host commit exceeds its reserved buffer");

    const auto inserted = tree_.insert(pages);
    std::vector<IndexCopy> copies;
    apply_split(inserted, copies);
    // Native host insertion discards any already-present radix prefix, even
    // when that prefix is device-only; only a new suffix gets host allocation.
    if (needed != 0) {
        nodes_[inserted.node].host = CacheSlot{};
        allocation.pages -= needed;
    }
    touch(inserted);
    return inserted.node;
}

void Cache::release(BufferId buffer) {
    const auto allocation = buffers_.at(buffer);
    if (allocation.tier == Tier::Device) reserved_device_pages_ -= allocation.unallocated_pages;
    return_pages(allocation.tier, allocation.pages);
    buffers_.erase(buffer);
}

void Cache::trim(BufferId buffer, std::size_t remaining_pages) {
    auto & allocation = buffers_.at(buffer);
    if (remaining_pages > allocation.pages) throw std::runtime_error("Cannot grow a reserved cache buffer by trimming it");
    return_pages(allocation.tier, allocation.pages - remaining_pages);
    allocation.pages = remaining_pages;
}

std::size_t Cache::evict_host_pages(std::size_t requested_pages) {
    std::size_t removed = 0;
    for (const auto node : eviction_order(Tier::Host)) {
        if (removed >= requested_pages) break;

        const auto pages = tree_.nodes()[node].pages.size();
        if (evict_host(node)) removed += pages;
    }
    return removed;
}

void Cache::pin_device(RadixId node) {
    while (node != 0) {
        ++nodes_[node].device_refs;
        node = tree_.nodes()[node].parent;
    }
}

void Cache::unpin_device(RadixId node) {
    while (node != 0) {
        --nodes_[node].device_refs;
        node = tree_.nodes()[node].parent;
    }
}

void Cache::pin_host(RadixId node) { ++nodes_[node].host_refs; }
void Cache::unpin_host(RadixId node) { --nodes_[node].host_refs; }

bool Cache::backup_required(RadixId node) const {
    const auto & state = nodes_[node];
    const auto parent = tree_.nodes()[node].parent;
    return config_.backup_after_hits && state.device && !state.host && state.hits >= *config_.backup_after_hits && (parent == 0 || nodes_[parent].host);
}

CopyStart Cache::begin_backup(RadixId node) {
    const auto pages = tree_.nodes()[node].pages.size();
    if (nodes_[node].host) return { CopyStatus::AlreadyPresent, std::nullopt, 0 };
    if (ready_layers(node, Tier::Device) != config_.layers) return { CopyStatus::SourceNotReady, std::nullopt, pages };
    if (!take_pages(Tier::Host, pages)) return { CopyStatus::InsufficientCapacity, std::nullopt, pages };

    const auto id = next_identity_++;
    publications_.emplace(id,
                          Publication{
                              Tier::Host,
                              { node },
                              0,
                              Copy{ CopyKind::Backup, node }
    });
    nodes_[node].host = CacheSlot{ id };
    pin_device(node);
    return { CopyStatus::Started, id, pages };
}

CopyStart Cache::begin_load(RadixId node) {
    if (nodes_[node].device) return { CopyStatus::AlreadyPresent, std::nullopt, 0 };

    std::vector<RadixId> loading;
    std::size_t pages = 0;
    bool sources_ready = true;
    for (auto id = node; id != 0 && !nodes_[id].device; id = tree_.nodes()[id].parent) {
        sources_ready &= ready_layers(id, Tier::Host) == config_.layers;
        loading.push_back(id);
        pages += tree_.nodes()[id].pages.size();
    }
    if (!sources_ready) return { CopyStatus::SourceNotReady, std::nullopt, pages };
    if (!take_pages(Tier::Device, pages)) return { CopyStatus::InsufficientCapacity, std::nullopt, pages };

    const auto id = next_identity_++;
    publications_.emplace(id,
                          Publication{
                              Tier::Device,
                              loading,
                              0,
                              Copy{ CopyKind::Load, node }
    });
    for (const auto destination : loading) nodes_[destination].device = CacheSlot{ id };
    pin_host(node);
    pin_device(node);
    return { CopyStatus::Started, id, pages };
}

std::size_t Cache::copy_segments(TransferId transfer) const { return publications_.at(transfer).nodes.size(); }

void Cache::publish_layer(TransferId transfer) { ++publications_.at(transfer).published_layers; }
void Cache::publish_all(TransferId transfer) { publications_.at(transfer).published_layers = config_.layers; }

void Cache::acknowledge(TransferId transfer) {
    const auto & publication = publications_.at(transfer);
    if (publication.published_layers != config_.layers) throw std::runtime_error("Cannot acknowledge an unpublished cache producer");

    for (const auto node : publication.nodes) slot(node, publication.destination)->producer.reset();
    if (publication.copy) {
        unpin_device(publication.copy->source_pin);
        if (publication.copy->kind == CopyKind::Load) unpin_host(publication.copy->source_pin);
    }
    publications_.erase(transfer);
}

std::vector<RadixId> Cache::eviction_order(Tier tier) const {
    using Candidate = std::pair<std::size_t, RadixId>;
    std::priority_queue<Candidate, std::vector<Candidate>, std::greater<Candidate>> eligible;
    std::vector<std::size_t> resident_children(nodes_.size(), 0);
    for (RadixId id = 1; id < nodes_.size(); ++id) {
        if (slot(id, tier)) ++resident_children[tree_.nodes()[id].parent];
    }
    const auto selectable = [&](RadixId id) {
        const auto & state = nodes_[id];
        return id != 0 && slot(id, tier) && !slot(id, tier)->producer && state.device_refs == 0
               && (tier == Tier::Device || (!state.device && state.host_refs == 0));
    };
    for (RadixId id = 1; id < nodes_.size(); ++id) {
        if (resident_children[id] == 0 && selectable(id)) eligible.emplace(nodes_[id].access_sequence, id);
    }

    std::vector<RadixId> result;
    while (!eligible.empty()) {
        const auto id = eligible.top().second;
        eligible.pop();
        result.push_back(id);

        const auto parent = tree_.nodes()[id].parent;
        if (--resident_children[parent] == 0 && selectable(parent)) eligible.emplace(nodes_[parent].access_sequence, parent);
    }
    return result;
}

void Cache::detach_empty_leaf(RadixId node) {
    if (!nodes_[node].device && !nodes_[node].host && tree_.nodes()[node].children.empty()) tree_.erase_leaf(node);
}

Eviction Cache::evict_device(RadixId node, std::span<const RadixId> pending) {
    if (node == 0 || !nodes_[node].device || nodes_[node].device->producer) return Eviction::Blocked;
    std::size_t batch_refs = 0;
    for (const auto selected : pending) {
        const auto & host = nodes_[selected].host;
        if (!host || !host->producer) continue;
        const auto & copy = publications_.at(*host->producer).copy;
        if (!copy || copy->kind != CopyKind::Backup) continue;
        for (auto ancestor = selected; ancestor != 0; ancestor = tree_.nodes()[ancestor].parent)
            if (ancestor == node) ++batch_refs;
    }
    if (nodes_[node].device_refs != batch_refs) return Eviction::Blocked;
    bool deferred = batch_refs != 0;
    for (const auto & [key, child] : tree_.nodes()[node].children) {
        if (!nodes_[child].device) continue;
        if (std::ranges::find(pending, child) == pending.end()) return Eviction::Blocked;
        deferred = true;
    }
    if (!nodes_[node].host && !config_.backup_after_hits) return Eviction::BackupRequired;
    if (!nodes_[node].host && !tree_.nodes()[node].children.empty()) return Eviction::Blocked;
    if (deferred) return Eviction::Deferred;

    return_pages(Tier::Device, tree_.nodes()[node].pages.size());
    nodes_[node].device.reset();
    detach_empty_leaf(node);
    return Eviction::Removed;
}

bool Cache::evict_host(RadixId node) {
    if (!nodes_[node].host || nodes_[node].host->producer || nodes_[node].device || nodes_[node].device_refs != 0 || nodes_[node].host_refs != 0) return false;
    for (const auto & [key, child] : tree_.nodes()[node].children) {
        if (nodes_[child].host) return false;
    }

    return_pages(Tier::Host, tree_.nodes()[node].pages.size());
    nodes_[node].host.reset();
    detach_empty_leaf(node);
    return true;
}

} // namespace markov::trace_graph::planning
