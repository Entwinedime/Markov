#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace markov {

struct KvResidence {
    std::optional<std::size_t> slot;

    // Allocation reserves addresses; execution dependencies determine when their contents are ready.
    std::int64_t allocated_tokens = 0;
    std::vector<std::size_t> pages;
};

} // namespace markov
