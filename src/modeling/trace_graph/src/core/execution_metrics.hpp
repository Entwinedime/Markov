#pragma once

#include <chrono>
#include <ctime>
#include <nlohmann/json.hpp>
#include <string_view>

namespace markov::trace_graph::core {

/** Host execution measurements, never simulated costs. Checkpoints partition
 * one scope; nested scopes are reported separately and must not be added. */
class ExecutionMetrics {
public:
    void checkpoint(std::string_view phase);
    nlohmann::json report() const;
    const nlohmann::json & phases() const { return phases_; }

private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point started_ = Clock::now(), previous_ = started_;
    std::clock_t cpu_started_ = std::clock(), cpu_previous_ = cpu_started_;
    nlohmann::json phases_ = nlohmann::json::object();
};

} // namespace markov::trace_graph::core
