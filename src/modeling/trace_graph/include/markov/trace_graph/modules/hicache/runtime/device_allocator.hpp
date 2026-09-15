#pragma once

#include <cstdint>
#include <optional>

namespace markov::trace_graph::modules::hicache::runtime {

/**
 * @brief Count-level projection of the SGLang device KV allocator.
 *
 * Tracks allocator availability and the free-index tensor slice. The radix tree
 * still owns logical residency; this ledger does not store device page identities.
 */
struct DeviceAllocatorLedger {
    bool initialized = false;
    bool need_sort = false;
    uint64_t capacity_pages = 0;
    uint64_t free_pages = 0;
    uint64_t release_pages = 0;
    // Missing after count-only reconciliation: no physical tensor operation was observed.
    std::optional<uint64_t> free_index_offset;

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

    /** @brief Reconciles allocator availability with committed radix and request ownership. */
    void reconcile_occupied_pages(uint64_t committed_pages, uint64_t request_owned_pages);
};

} // namespace markov::trace_graph::modules::hicache::runtime
