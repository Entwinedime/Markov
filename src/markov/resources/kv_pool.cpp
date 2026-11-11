#include "markov/resources/kv_pool.hpp"

#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <utility>


namespace markov {

KvPool::KvPool(std::size_t request_count, const ServingSettings & settings)
    : request_capacity_(settings.max_running_requests),
      page_size_(settings.page_size),
      capacity_{ static_cast<std::size_t>(settings.max_running_requests), static_cast<std::size_t>(settings.max_total_tokens / settings.page_size) },
      residences_(request_count),
      free_slots_(capacity_.slots),
      free_pages_(capacity_.pages),
      ordered_pages_(static_cast<std::int64_t>(free_pages_.size())),
      prefixes_(settings.page_size) {
    std::iota(free_slots_.begin(), free_slots_.end(), std::size_t{ 0 });
    std::iota(free_pages_.begin(), free_pages_.end(), std::size_t{ 0 });
}

std::int64_t KvPool::pages_for(std::int64_t tokens) const { return tokens == 0 ? 0 : (tokens - 1) / page_size_ + 1; }
const KvResidence & KvPool::residence(std::size_t request) const { return residences_.at(request); }
const std::deque<std::size_t> & KvPool::free_slots() const { return free_slots_; }
const std::deque<std::size_t> & KvPool::free_pages() const { return free_pages_; }
const KvCapacity & KvPool::capacity() const { return capacity_; }
PrefixIndex & KvPool::prefixes() { return prefixes_; }
const PrefixIndex & KvPool::prefixes() const { return prefixes_; }

void KvPool::declare_resources(std::size_t slots, PageReuse reuse) {
    if (slots < static_cast<std::size_t>(request_capacity_) || reuse_)
        throw std::invalid_argument("resources must be declared once and cover running requests");
    free_slots_.resize(slots);
    capacity_.slots = slots;
    std::iota(free_slots_.begin(), free_slots_.end(), std::size_t{ 0 });
    reuse_ = reuse;
}

void KvPool::bind_prefix(std::size_t request, std::size_t node, std::int64_t tokens) {
    auto & resident = residences_.at(request);
    auto pages = prefixes_.binding(node);
    const auto cached_tokens = static_cast<std::int64_t>(pages.size()) * page_size_;
    if (tokens < cached_tokens || (resident.allocated_tokens != 0 && tokens != resident.allocated_tokens))
        throw std::invalid_argument("prefix binding differs from the live request extent");

    // A partial tail belongs to the request; only whole cached pages replace its existing bindings.
    if (tokens > cached_tokens) {
        if (tokens > resident.allocated_tokens) throw std::invalid_argument("prefix binding has no allocated tail");
        pages.insert(pages.end(), resident.pages.begin() + static_cast<std::ptrdiff_t>(pages.size()), resident.pages.end());
    }
    resident.pages = std::move(pages);
    resident.allocated_tokens = tokens;
}

KvAllocation KvPool::allocate(std::span<const KvDemand> demands) {
    const auto reuse = reuse_.value();
    auto allocation = plan(demands);

    // Deferred pages join the ordered prefix only when that prefix cannot satisfy the demand.
    if (reuse == PageReuse::DeferredSorted && allocation.allocated_pages > ordered_pages_) {
        allocation.ordering = PageOrdering{ ordered_pages_, static_cast<std::int64_t>(free_pages_.size()) - ordered_pages_ };
        std::ranges::sort(free_pages_);
        ordered_pages_ = static_cast<std::int64_t>(free_pages_.size());
    }
    auto next = free_pages_.begin();
    for (auto & member : allocation.members) {
        for (auto & page : member.new_pages) page = *next++;
    }
    reserve(allocation);
    return allocation;
}

KvAllocation KvPool::plan(std::span<const KvDemand> demands) const {
    KvAllocation allocation;
    std::size_t new_slots = 0;
    for (const auto & [request, tokens] : demands) {
        const auto & resident = residence(request);
        const auto slot = resident.slot ? *resident.slot : free_slots_.at(new_slots++);
        KvReservation member{ request, slot, resident.allocated_tokens, tokens, {} };

        // Check the complete demand before selecting identities or changing either free list.
        const auto pages = pages_for(member.context_tokens + member.new_tokens) - static_cast<std::int64_t>(resident.pages.size());
        if (allocation.allocated_pages + pages > static_cast<std::int64_t>(free_pages_.size()))
            throw std::invalid_argument("allocation exceeds available KV pages");
        member.new_pages.resize(static_cast<std::size_t>(pages));
        allocation.allocated_pages += pages;
        allocation.members.push_back(std::move(member));
    }
    return allocation;
}

std::vector<KvPageRange> KvPool::ranges(std::size_t request, std::int64_t begin, std::int64_t end) const {
    const auto & resident = residence(request);
    if (begin < 0 || end < begin || end > resident.allocated_tokens) throw std::invalid_argument("KV access is outside the request's allocated token range");

    // Prefix bindings already share physical pages; preserve sub-page boundaries instead of widening the access.
    std::vector<KvPageRange> result;
    for (auto token = begin; token < end;) {
        const auto offset = token % page_size_;
        const auto length = std::min(page_size_ - offset, end - token);
        result.push_back({ resident.pages.at(static_cast<std::size_t>(token / page_size_)), offset, offset + length });
        token += length;
    }
    return result;
}

std::vector<std::int64_t> KvPool::indices(std::size_t request, std::int64_t begin, std::int64_t end) const {
    const auto pages = ranges(request, begin, end);
    std::vector<std::int64_t> result;
    result.reserve(static_cast<std::size_t>(end - begin));
    for (const auto & range : pages) {
        const auto base = static_cast<std::int64_t>(range.page) * page_size_;
        for (auto offset = range.begin; offset < range.end; ++offset) result.push_back(base + offset);
    }
    return result;
}

void KvPool::reserve(const KvAllocation & allocation) {
    for (const auto & member : allocation.members) {
        auto & state = residences_.at(member.request);
        if (!state.slot) {
            state.slot = member.slot;
            free_slots_.pop_front();
        }
        state.pages.insert(state.pages.end(), member.new_pages.begin(), member.new_pages.end());
        state.allocated_tokens = member.context_tokens + member.new_tokens;
    }
    free_pages_.erase(free_pages_.begin(), free_pages_.begin() + allocation.allocated_pages);
    ordered_pages_ -= allocation.allocated_pages;
}

void KvPool::release(std::size_t request) {
    auto & state = residences_.at(request);
    free_slots_.push_back(state.slot.value());
    state = {};
}

void KvPool::return_pages(std::span<const std::size_t> pages) {

    // Control returns logical ownership here; execution dependencies separately protect outstanding accesses.
    const auto reuse = reuse_.value();
    const auto position = reuse == PageReuse::DeferredSorted ? free_pages_.end() : free_pages_.begin();
    free_pages_.insert(position, pages.begin(), pages.end());
    if (reuse == PageReuse::Prepend) ordered_pages_ += static_cast<std::int64_t>(pages.size());
}

} // namespace markov
