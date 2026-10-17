#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace markov {

struct Request {
    std::string id;
    std::int64_t arrival_time_ns;
    std::variant<std::vector<std::uint32_t>, std::string> input;
    std::uint32_t output_tokens;
};

} // namespace markov
