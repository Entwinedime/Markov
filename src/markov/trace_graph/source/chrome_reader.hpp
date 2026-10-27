#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

namespace markov::trace_graph::source {

class RecordConsumer {
public:
    virtual ~RecordConsumer() = default;
    virtual void record(std::size_t position, const nlohmann::json & event) = 0;
};

// Stream one Chrome event at a time. The consumer must not retain event views.
// Accept the two Chrome containers: a traceEvents object or an event array.
std::size_t read_chrome_trace(const std::filesystem::path & path, RecordConsumer & consumer);

// Chrome durations and timestamps are microseconds; strings retain fractional
// precision. Reject non-finite, negative or overflowing external measurements.
std::int64_t nanoseconds_from_us(const nlohmann::json & value);
std::int64_t nanoseconds_from_ms(const nlohmann::json & value);
std::string scalar_identity(const nlohmann::json & value);

} // namespace markov::trace_graph::source
