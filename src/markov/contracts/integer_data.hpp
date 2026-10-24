#pragma once

#include <cstdint>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace markov {

struct IntegerRun {
    std::int64_t begin;
    std::int64_t count;

    bool operator==(const IntegerRun &) const = default;
};

// A one-dimensional integer tensor, stored as concatenated consecutive ranges.
class IntegerData {
public:
    void append(std::int64_t begin, std::int64_t count = 1);
    void append(IntegerData other);
    IntegerData slice(std::int64_t offset, std::int64_t count) const;

    std::int64_t size() const;
    nlohmann::json document() const;
    bool operator==(const IntegerData &) const = default;

private:
    std::vector<IntegerRun> runs_;
    std::int64_t size_ = 0;
};

} // namespace markov
