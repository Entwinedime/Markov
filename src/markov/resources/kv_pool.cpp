#include "markov/resources/kv_pool.hpp"

#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <utility>

#include "markov/resources/trace.hpp"

namespace markov {

KvPool::KvPool(std::size_t request_count, const ServingSettings & settings, PageReuse reuse, ResourceTrace * trace, std::string pool)
    : request_capacity_(settings.max_running_requests),
      page_size_(settings.page_size),
      capacity_{ static_cast<std::size_t>(settings.max_running_requests), static_cast<std::size_t>(settings.max_total_tokens / settings.page_size) },
      residences_(request_count),
      free_slots_(capacity_.slots),
      free_pages_(capacity_.pages),
      reuse_(reuse),
      ready_pages_(static_cast<std::int64_t>(free_pages_.size())),
      trace_(trace),
      pool_(std::move(pool)),
      prefixes_(settings.page_size, trace_, pool_) {
    std::iota(free_slots_.begin(), free_slots_.end(), std::size_t{ 0 });
    std::iota(free_pages_.begin(), free_pages_.end(), std::size_t{ 0 });
    if (trace_)
        trace_->append(pool_,
                       "pool_init",
                       {
                           { "slots", capacity_.slots },
                           { "pages", capacity_.pages }
        });
}

std::int64_t KvPool::pages_for(std::int64_t tokens) const { return tokens == 0 ? 0 : (tokens - 1) / page_size_ + 1; }
std::int64_t KvPool::page_size() const { return page_size_; }
const KvResidence & KvPool::residence(std::size_t request) const { return residences_.at(request); }
std::span<const KvResidence> KvPool::residences() const { return residences_; }
const std::deque<std::size_t> & KvPool::free_slots() const { return free_slots_; }
const std::deque<std::size_t> & KvPool::free_pages() const { return free_pages_; }
std::int64_t KvPool::deferred_pages() const { return static_cast<std::int64_t>(free_pages_.size()) - ready_pages_; }
const KvCapacity & KvPool::capacity() const { return capacity_; }
PrefixIndex & KvPool::prefixes() { return prefixes_; }
const PrefixIndex & KvPool::prefixes() const { return prefixes_; }

void KvPool::declare_slots(std::size_t capacity) {
    if (capacity < static_cast<std::size_t>(request_capacity_)
        || std::ranges::any_of(residences_, [](const auto & resident) { return resident.slot.has_value(); }))
        throw std::invalid_argument("slot capacity must cover running requests and be declared before allocation");
    free_slots_.resize(capacity);
    capacity_.slots = capacity;
    std::iota(free_slots_.begin(), free_slots_.end(), std::size_t{ 0 });
    if (trace_)
        trace_->append(pool_,
                       "slots",
                       {
                           { "slots", capacity_.slots }
        });
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
    if (trace_)
        trace_->append(pool_,
                       "bind",
                       {
                           { "request",        request },
                           {    "node",           node },
                           {  "tokens",         tokens },
                           {   "pages", resident.pages }
        });
}

Batch KvPool::plan(Phase phase, std::span<const KvDemand> demands) const {
    Batch batch{ phase, request_capacity_, static_cast<std::int64_t>(free_pages_.size()), 0, {}, deferred_pages(), static_cast<std::int64_t>(capacity_.slots) };
    std::size_t new_slots = 0;
    for (const auto & [request, tokens] : demands) {
        const auto & resident = residence(request);
        const auto slot = resident.slot ? *resident.slot : free_slots_.at(new_slots++);
        BatchMember member{ request, slot, resident.allocated_tokens, tokens, resident.pages.empty() ? std::nullopt : std::optional(resident.pages.back()),
                            {},      phase };

        // Selection describes an allocation; reserve commits it when control submits the work.
        const auto pages = pages_for(resident.allocated_tokens + tokens) - static_cast<std::int64_t>(resident.pages.size());
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
        state.allocated_tokens = member.context_tokens + member.new_tokens;
        if (trace_)
            trace_->append(pool_,
                           "reserve",
                           {
                               { "request",         member.request },
                               {    "slot",            member.slot },
                               {  "tokens", state.allocated_tokens },
                               {   "pages",       member.new_pages }
            });
    }
    free_pages_.erase(free_pages_.begin(), free_pages_.begin() + batch.allocated_pages);
    ready_pages_ -= batch.allocated_pages;
}

void KvPool::release(std::size_t request) {
    auto & state = residences_.at(request);
    free_slots_.push_back(state.slot.value());
    state = {};
    if (trace_)
        trace_->append(pool_,
                       "release",
                       {
                           { "request", request }
        });
}

void KvPool::return_pages(std::span<const std::size_t> pages) {

    // The native release graph supplies order; this pool remains the sole owner of reusable identities.
    const auto position = reuse_ == PageReuse::DeferredSorted ? free_pages_.end() : free_pages_.begin();
    free_pages_.insert(position, pages.begin(), pages.end());
    if (reuse_ == PageReuse::Immediate) ready_pages_ += static_cast<std::int64_t>(pages.size());
    if (trace_ && !pages.empty())
        trace_->append(pool_,
                       "return_pages",
                       {
                           { "pages", pages }
        });
}

} // namespace markov
