#include "markov/controller/kv_pool.hpp"

#include <algorithm>
#include <numeric>

namespace markov {

KvPool::KvPool(std::size_t request_count, const ServingSettings & settings, PageReuse reuse)
    : page_size_(settings.page_size),
      residences_(request_count),
      free_slots_(static_cast<std::size_t>(settings.max_running_requests)),
      free_pages_(static_cast<std::size_t>(settings.max_total_tokens / settings.page_size)),
      reuse_(reuse),
      ready_pages_(static_cast<std::int64_t>(free_pages_.size())) {
    std::iota(free_slots_.begin(), free_slots_.end(), std::size_t{ 0 });
    std::iota(free_pages_.begin(), free_pages_.end(), std::size_t{ 0 });
}

std::int64_t KvPool::pages_for(std::int64_t tokens) const { return tokens == 0 ? 0 : (tokens - 1) / page_size_ + 1; }
const KvResidence & KvPool::residence(std::size_t request) const { return residences_.at(request); }
const std::deque<std::size_t> & KvPool::free_slots() const { return free_slots_; }
const std::deque<std::size_t> & KvPool::free_pages() const { return free_pages_; }
std::int64_t KvPool::deferred_pages() const { return static_cast<std::int64_t>(free_pages_.size()) - ready_pages_; }

void KvPool::assign_pages(Batch & batch) const {
    if (batch.allocated_pages <= ready_pages_) return;

    // Deferred identities become ordered together only when the ready prefix cannot satisfy this allocation.
    auto available = free_pages_;
    std::ranges::sort(available);
    auto next = available.begin();
    for (auto & member : batch.members) {
        for (auto & page : member.new_pages) page = *next++;
    }
}

void KvPool::reserve(const Batch & batch) {
    if (batch.allocated_pages > ready_pages_) {
        std::ranges::sort(free_pages_);
        ready_pages_ = static_cast<std::int64_t>(free_pages_.size());
    }
    for (const auto & member : batch.members) {
        auto & state = residences_.at(member.request);
        if (!state.slot) {
            state.slot = member.slot;
            free_slots_.pop_front();
        }
        state.pages.insert(state.pages.end(), member.new_pages.begin(), member.new_pages.end());
    }
    free_pages_.erase(free_pages_.begin(), free_pages_.begin() + batch.allocated_pages);
    ready_pages_ -= batch.allocated_pages;
}

void KvPool::advance(std::size_t request, std::int64_t tokens) { residences_.at(request).tokens = tokens; }

void KvPool::release(std::size_t request) {
    auto & state = residences_.at(request);
    free_slots_.push_back(state.slot.value());
    state = {};
}

void KvPool::return_pages(std::span<const std::size_t> pages) {

    // The native release graph supplies order; this pool remains the sole owner of reusable identities.
    const auto position = reuse_ == PageReuse::DeferredSorted ? free_pages_.end() : free_pages_.begin();
    free_pages_.insert(position, pages.begin(), pages.end());
    if (reuse_ == PageReuse::Immediate) ready_pages_ += static_cast<std::int64_t>(pages.size());
}

} // namespace markov
