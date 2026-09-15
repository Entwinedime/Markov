#include "markov/trace_graph/modules/hicache/runtime/device_allocator.hpp"
#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>

namespace markov::trace_graph::modules::hicache::runtime {

/**
 * @brief Initializes the count-level projection of the device allocator.
 *
 * The ledger reconstructs free/release queue visibility but does not own page
 * identity. Page identity remains derived from the radix tree and capacity index.
 */
void DeviceAllocatorLedger::configure(uint64_t pages, bool sort_required) {
    if (initialized && capacity_pages == pages && need_sort == sort_required) return;
    initialized = true;
    need_sort = sort_required;
    capacity_pages = pages;
    free_pages = pages;
    release_pages = 0;
    free_index_offset = pages > 0 ? std::optional<uint64_t>{0} : std::nullopt;
}

uint64_t DeviceAllocatorLedger::available_pages() const {
    return core::checked_add_u64(free_pages, release_pages, "HiCache device allocator availability exceeds uint64 range");
}

/** @brief Reports whether a page request triggers the SGLang eviction gate. */
bool DeviceAllocatorLedger::should_evict(uint64_t requested_pages) const {
    return initialized && capacity_pages > 0 && requested_pages > 0 && available_pages() < requested_pages;
}

/** @brief Merges the pending release queue into allocator free pages. */
void DeviceAllocatorLedger::merge_release_pages() {
    if (release_pages > 0) free_index_offset = 0;
    free_pages = core::checked_add_u64(free_pages, release_pages, "HiCache device allocator free page count exceeds uint64 range");
    release_pages = 0;
}

/**
 * @brief Reconstructs conditional release-queue synchronization before extend.
 *
 * The paged allocator includes batch overhead in its pressure gate. This count-level
 * projection preserves that timing so cleanup is not driven only by final occupancy.
 */
void DeviceAllocatorLedger::merge_before_extend(uint64_t extend_tokens, uint64_t batch_size, uint64_t page_size) {
    if (!need_sort || page_size == 0) return;
    const auto needed_pages =
        page_size == 1
            ? extend_tokens
            : core::checked_add_u64(core::checked_add_u64(extend_tokens / page_size, batch_size, "HiCache extend allocator gate exceeds uint64 range"),
                                    1,
                                    "HiCache extend allocator gate exceeds uint64 range");
    if (needed_pages > free_pages) merge_release_pages();
}

/** @brief Makes pending releases visible when required by the page request. */
void DeviceAllocatorLedger::merge_before_page_allocation(uint64_t requested_pages) {
    if (need_sort && requested_pages > free_pages) merge_release_pages();
}

bool DeviceAllocatorLedger::can_allocate(uint64_t pages) const { return capacity_pages == 0 || pages <= free_pages; }

uint64_t DeviceAllocatorLedger::allocate(uint64_t pages) {
    if (capacity_pages == 0) return pages;
    const auto consumed = std::min(pages, free_pages);
    if (consumed != pages) free_index_offset.reset();
    else if (free_index_offset) *free_index_offset = core::checked_add_u64(*free_index_offset, consumed, "HiCache allocator slice offset exceeds uint64 range");
    free_pages -= consumed;
    return consumed;
}

/** @brief Releases pages directly or through the queue according to `need_sort`. */
uint64_t DeviceAllocatorLedger::release(uint64_t pages) {
    if (pages == 0 || capacity_pages == 0) return 0;
    const auto room = capacity_pages > available_pages() ? capacity_pages - available_pages() : 0;
    const auto released = std::min(pages, room);
    if (released != pages) free_index_offset.reset();
    else if (!need_sort) free_index_offset = 0;
    if (need_sort) release_pages = core::checked_add_u64(release_pages, released, "HiCache release queue exceeds uint64 range");
    else free_pages = core::checked_add_u64(free_pages, released, "HiCache allocator free page count exceeds uint64 range");
    return released;
}

/**
 * @brief Restores the allocator/tree ownership invariant at a lifecycle commit.
 *
 * Device pages are owned either by the committed radix tree or by requests whose
 * allocation has not yet become radix-visible.  Reconciliation is derived from
 * those two ownership ledgers; it does not depend on a workload or configuration
 * name.  An existing delayed-release queue is preserved when the target allocator
 * requires sorting before reuse.
 */
void DeviceAllocatorLedger::reconcile_occupied_pages(uint64_t committed_pages, uint64_t request_owned_pages) {
    if (!initialized || capacity_pages == 0) return;
    const auto owned_pages = core::checked_add_u64(committed_pages, request_owned_pages, "HiCache device ownership exceeds uint64 range");
    const auto bounded_owned_pages = std::min(owned_pages, capacity_pages);
    const auto total_available_pages = capacity_pages - bounded_owned_pages;
    if (total_available_pages != available_pages()) free_index_offset.reset();
    release_pages = std::min(release_pages, total_available_pages);
    free_pages = total_available_pages - release_pages;
}

} // namespace markov::trace_graph::modules::hicache::runtime
