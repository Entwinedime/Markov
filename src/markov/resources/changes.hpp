#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace markov {

// Logical returns invalidate old contents; pending accesses still protect physical reuse.
struct ResourceChanges {
    std::vector<std::size_t> returned_pages;
    std::vector<std::size_t> returned_slots;
    std::vector<std::pair<std::string, std::size_t>> returned_fields;
};

} // namespace markov
