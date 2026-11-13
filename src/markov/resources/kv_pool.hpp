#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "markov/contracts/serving.hpp"
#include "markov/resources/prefix_index.hpp"

namespace markov {

enum class PageReuse { Prepend, DeferredSorted };

struct KvResidence {
    std::optional<std::size_t> slot;

    // Allocation reserves addresses; execution dependencies determine when their contents are ready.
    std::int64_t allocated_tokens = 0;
    std::vector<std::size_t> pages;
};

struct KvDemand {
    std::size_t request;
    std::int64_t tokens;
};

struct KvCapacity {
    std::size_t slots;
    std::size_t pages;
};

// Offsets are tokens within one physical page; shared prefixes retain the same page identity.
struct KvPageRange {
    std::size_t page;
    std::int64_t begin;
    std::int64_t end;
};

struct KvReservation {
    std::size_t request;
    std::size_t slot;
    std::int64_t context_tokens;
    std::int64_t new_tokens;
    std::vector<std::size_t> new_pages;
};

struct PageOrdering {
    std::int64_t ready_pages;
    std::int64_t released_pages;
};

struct KvAllocation {
    std::vector<KvReservation> members;
    std::int64_t allocated_pages = 0;
    std::optional<PageOrdering> ordering;
};

// Shared resource ownership is independent of control policy and request progress.
class KvPool {
public:
    KvPool(std::size_t request_count, const ServingSettings & settings);

    std::int64_t pages_for(std::int64_t tokens) const;
    const KvResidence & residence(std::size_t request) const;
    const std::deque<std::size_t> & free_slots() const;
    const std::deque<std::size_t> & free_pages() const;
    const KvCapacity & capacity() const;
    PrefixIndex & prefixes();
    const PrefixIndex & prefixes() const;
    KvAllocation allocate(std::span<const KvDemand> demands);
    std::vector<KvPageRange> ranges(std::size_t request, std::int64_t begin, std::int64_t end) const;
    std::vector<std::int64_t> indices(std::size_t request, std::int64_t begin, std::int64_t end) const;

    void declare_resources(std::size_t slots, PageReuse reuse);
    void bind_prefix(std::size_t request, std::size_t node, std::int64_t tokens);
    void release(std::size_t request);
    void return_pages(std::span<const std::size_t> pages);

private:
    KvAllocation plan(std::span<const KvDemand> demands) const;
    void reserve(const KvAllocation & allocation);

    std::int64_t request_capacity_;
    std::int64_t page_size_;
    KvCapacity capacity_;
    std::vector<KvResidence> residences_;
    std::deque<std::size_t> free_slots_;
    std::deque<std::size_t> free_pages_;
    std::optional<PageReuse> reuse_;
    std::int64_t ordered_pages_;
    PrefixIndex prefixes_;
};

} // namespace markov
