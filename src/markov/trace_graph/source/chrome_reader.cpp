#include "markov/trace_graph/source/chrome_reader.hpp"

#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace markov::trace_graph::source {

std::size_t read_chrome_trace(const std::filesystem::path & path, RecordConsumer & consumer) {
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("Cannot read trace: " + path.string());

    using Json = nlohmann::json;
    using Event = Json::parse_event_t;
    std::size_t count = 0;
    int event_array_depth = -1;
    bool event_key = false;
    bool found_events = false;

    // Discard each completed event from the parser's parent array. Memory is
    // bounded by one record and root metadata, rather than the complete file.
    const auto callback = [&](int depth, Event event, Json & value) {
        if (event == Event::key && depth == 1) event_key = value == "traceEvents";
        if (event == Event::array_start && (depth == 0 || (depth == 1 && event_key))) {
            event_array_depth = depth;
            found_events = true;
            event_key = false;
        }
        if (event == Event::object_end && event_array_depth >= 0 && depth == event_array_depth + 1) {
            consumer.record(count++, value);
            return false;
        }
        if (event == Event::array_end && depth == event_array_depth) event_array_depth = -1;
        return true;
    };
    try {
        (void)Json::parse(stream, callback);
    }
    catch (const Json::exception & error) {
        throw std::runtime_error(path.string() + ": " + error.what());
    }
    if (!found_events) throw std::runtime_error("No Chrome traceEvents array: " + path.string());

    return count;
}

namespace {

std::int64_t read_time(const nlohmann::json & value, long double scale_ns) {
    const auto text = value.is_string() ? value.get<std::string>() : value.dump();
    std::size_t consumed = 0;
    const auto nanoseconds = std::stold(text, &consumed) * scale_ns;
    if (consumed != text.size() || !std::isfinite(nanoseconds) || nanoseconds < 0
        || nanoseconds > static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        throw std::runtime_error("Invalid measured time: " + text);

    return static_cast<std::int64_t>(std::round(nanoseconds));
}

} // namespace

std::int64_t nanoseconds_from_us(const nlohmann::json & value) { return read_time(value, 1'000); }
std::int64_t nanoseconds_from_ms(const nlohmann::json & value) { return read_time(value, 1'000'000); }

std::string scalar_identity(const nlohmann::json & value) { return value.is_string() ? value.get<std::string>() : value.dump(); }

} // namespace markov::trace_graph::source
