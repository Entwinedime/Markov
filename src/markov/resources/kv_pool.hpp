#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "markov/contracts/serving.hpp"

namespace markov {

struct KvResidence {
    std::optional<std::size_t> slot;
    std::int64_t tokens = 0;
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

// Shared resource ownership is independent of control policy and request progress.
class KvPool {
public:
    KvPool(std::size_t request_count, const ServingSettings & settings, PageReuse reuse = PageReuse::Immediate);

    std::int64_t pages_for(std::int64_t tokens) const;
    const KvResidence & residence(std::size_t request) const;
    const std::deque<std::size_t> & free_slots() const;
    const std::deque<std::size_t> & free_pages() const;
    std::int64_t deferred_pages() const;
    const KvCapacity & capacity() const;
    Batch plan(Phase phase, std::span<const KvDemand> demands = {}) const;
    std::vector<std::int64_t> indices(std::size_t request, std::int64_t begin, std::int64_t end) const;

    void declare_slots(std::size_t capacity);
    void reserve(const Batch & batch);
    void advance(std::size_t request, std::int64_t tokens);
    void release(std::size_t request);
    void return_pages(std::span<const std::size_t> pages);

private:
    void assign_pages(Batch & batch) const;

    std::int64_t request_capacity_;
    std::int64_t page_size_;
    KvCapacity capacity_;
    std::vector<KvResidence> residences_;
    std::deque<std::size_t> free_slots_;
    std::deque<std::size_t> free_pages_;
    PageReuse reuse_;
    std::int64_t ready_pages_;
};

} // namespace markov
