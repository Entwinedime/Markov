#pragma once

#include <cstddef>
#include <deque>
#include <map>
#include <optional>
#include <set>

namespace markov {

// A physical row can be reserved before it is associated with a request.
class SlotPool {
public:
    explicit SlotPool(std::size_t capacity);
    std::size_t available() const;
    std::optional<std::size_t> allocate();
    void bind(std::size_t request, std::size_t slot);
    void release(std::size_t slot);
    std::size_t slot(std::size_t request) const;

private:
    std::deque<std::size_t> free_;
    std::set<std::size_t> allocated_;
    std::map<std::size_t, std::size_t> bindings_;
};

} // namespace markov
