#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/contracts/serving.hpp"

namespace markov {

// Input availability and receive cohorts are independent of the batches chosen by native control.
class RequestDelivery {
public:
    RequestDelivery(std::span<const Request> requests, const nlohmann::json * ingress);
    std::optional<std::int64_t> next_ns() const;
    bool ready(std::int64_t now_ns) const;
    std::vector<std::size_t> take(std::int64_t now_ns);
    const std::set<std::size_t> & delivered() const;

private:
    struct Round {
        std::int64_t time_ns;
        std::vector<std::size_t> requests;
    };

    std::vector<Round> rounds_;
    std::size_t next_ = 0;
    std::set<std::size_t> delivered_;
    bool observed_;
};

} // namespace markov
