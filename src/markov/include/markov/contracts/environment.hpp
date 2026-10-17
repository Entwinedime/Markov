#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace markov {

struct Environment {
    std::string service;
    std::vector<std::uint32_t> devices;
};

} // namespace markov
