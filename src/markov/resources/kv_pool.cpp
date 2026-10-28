#include "markov/resources/kv_pool.hpp"

#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace markov {

KvPool::KvPool(std::size_t request_count, const ServingSettings & settings, PageReuse reuse)
    : request_capacity_(settings.max_running_requests),
      page_size_(settings.page_size),
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

Batch KvPool::plan(Phase phase, std::span<const KvDemand> demands) const {
    Batch batch{ phase, request_capacity_, static_cast<std::int64_t>(free_pages_.size()), 0, {}, deferred_pages() };
    std::size_t new_slots = 0;
    for (const auto & [request, tokens] : demands) {
        const auto & resident = residence(request);
        const auto slot = resident.slot ? *resident.slot : free_slots_.at(new_slots++);
        BatchMember member{ request, slot, resident.tokens, tokens, resident.pages.empty() ? std::nullopt : std::optional(resident.pages.back()), {} };

        // Selection describes an allocation; reserve commits it when control submits the work.
        const auto pages = pages_for(resident.tokens + tokens) - static_cast<std::int64_t>(resident.pages.size());
        if (batch.allocated_pages + pages > static_cast<std::int64_t>(free_pages_.size())) throw std::invalid_argument("batch exceeds available KV pages");
        for (std::int64_t index = 0; index < pages; ++index)
            member.new_pages.push_back(free_pages_.at(static_cast<std::size_t>(batch.allocated_pages + index)));
        batch.allocated_pages += pages;
        batch.members.push_back(std::move(member));
    }

    assign_pages(batch);
    return batch;
}

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

std::vector<std::int64_t> KvPool::indices(std::size_t request, std::int64_t begin, std::int64_t end) const {
    const auto & pages = residence(request).pages;
    std::vector<std::int64_t> result;
    for (auto token = begin; token < end; ++token) {
        const auto page = pages.at(static_cast<std::size_t>(token / page_size_));
        result.push_back(static_cast<std::int64_t>(page) * page_size_ + token % page_size_);
    }
    return result;
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
