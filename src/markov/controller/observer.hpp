#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <nlohmann/json.hpp>

namespace markov {

// Observe native control decisions without maintaining another copy of their state.
class ControlObserver {
public:
    virtual ~ControlObserver() = default;
    virtual void initialized(const nlohmann::json & instances, const nlohmann::json & requests) = 0;
    virtual void input_read(const nlohmann::json & identity, std::span<const std::size_t> requests, std::int64_t time_ns) = 0;
    virtual void observed_values(std::string_view group, std::size_t boundary, const nlohmann::json & values, std::int64_t time_ns) = 0;
    virtual void queried(std::string_view group, std::size_t boundary, const nlohmann::json & result, std::int64_t time_ns) = 0;
    virtual void completion_read(std::string_view group, std::size_t boundary, std::string_view signal, std::size_t event,
                                 bool result, std::int64_t time_ns) = 0;
    virtual void notification_declared(const nlohmann::json & identity, std::size_t event) = 0;
    virtual void notification_ready(const nlohmann::json & identity, std::size_t event,
                                    std::span<const std::size_t> prerequisites, std::int64_t time_ns) = 0;
};

} // namespace markov
