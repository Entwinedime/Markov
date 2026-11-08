#include "markov/resources/slot_pool.hpp"

#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace markov {

SlotPool::SlotPool(std::size_t capacity) : free_(capacity) {
    if (!capacity) throw std::invalid_argument("slot pool capacity must be positive");
    std::iota(free_.begin(), free_.end(), std::size_t{0});
}

std::size_t SlotPool::available() const { return free_.size(); }

std::optional<std::size_t> SlotPool::allocate() {
    if (free_.empty()) return std::nullopt;
    const auto slot = free_.front();
    free_.pop_front();
    allocated_.insert(slot);
    return slot;
}

void SlotPool::bind(std::size_t request, std::size_t slot) {
    if (!allocated_.contains(slot)) throw std::invalid_argument("request binding requires a reserved slot");
    for (const auto & [owner, address] : bindings_) {
        if ((owner == request || address == slot) && (owner != request || address != slot))
            throw std::invalid_argument("slot binding conflicts with a live owner");
    }
    bindings_[request] = slot;
}

void SlotPool::release(std::size_t slot) {
    if (!allocated_.erase(slot)) throw std::invalid_argument("returned slot was not reserved");
    std::erase_if(bindings_, [slot](const auto & binding) { return binding.second == slot; });
    free_.push_back(slot);
}

std::size_t SlotPool::slot(std::size_t request) const { return bindings_.at(request); }

} // namespace markov
