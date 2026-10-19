#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace markov {

class CostDistribution {
public:
    void observe(std::int64_t duration_ns);
    nlohmann::json describe() const;
    std::span<const std::int64_t> samples() const { return values_; }

private:
    std::vector<std::int64_t> values_;
};

class CostErrors {
public:
    void observe(std::int64_t estimate, std::int64_t observed);
    nlohmann::json describe() const;

private:
    std::size_t count_ = 0;
    long double observed_ns_ = 0;
    long double absolute_ns_ = 0;
    long double signed_ns_ = 0;
    std::int64_t maximum_ns_ = 0;
};

} // namespace markov
