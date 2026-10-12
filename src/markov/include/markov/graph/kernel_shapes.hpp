#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace markov {

// Vendor tensor slots retain omitted optional operands across finite layout changes.
class KernelShapes {
public:
    explicit KernelShapes(std::string_view text);
    std::string encode() const;

    std::vector<std::vector<std::int64_t>> tensors;
};

} // namespace markov
