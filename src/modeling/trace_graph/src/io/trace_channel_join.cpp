/**
 * @file
 * @brief Implements timestamp alignment and joining for custom trace channels.
 *
 * Joining happens in memory before DAG construction. It preserves lazy argument
 * storage and never materializes a merged trace file.
 */
#include "trace_channel_join.hpp"

#include "markov/trace_graph/core/logger.hpp"
#include "markov/trace_graph/core/numeric.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <limits>
#include <ranges>
#include <unordered_map>

namespace markov::trace_graph::io::detail {

namespace {

using core::Logger;
using core::TraceEvent;
using Json = nlohmann::json;

struct CustomEventGroup {
    std::vector<uint64_t> timestamps;
    std::vector<const TraceEvent *> events;
    std::vector<bool> used;
};

std::string lowercase(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

bool is_standalone_event(const TraceEvent & event) {
    if (event.name == "CPUInfer::submit" || event.name == "CPUInfer::sync") return true;
    if (event.name.starts_with("HiCache::") || event.name.starts_with("hicache_")) return true;
    const auto domain = lowercase(event.arg("domain", event.cat));
    return domain == "hicache" || domain == "cache_io" || domain == "python_probe";
}

uint64_t earliest_timestamp(const std::vector<TraceEvent> & events) {
    if (events.empty()) return 0;
    return std::ranges::min(events, {}, &TraceEvent::ts).ts;
}

std::string cann_pid(const std::vector<TraceEvent> & events) {
    for (const auto & event : events) {
        if (event.ph == 'M' && event.name == "process_name" && event.arg("name") == "CANN") return event.pid;
    }
    return "0";
}

std::string custom_key(const TraceEvent & event) { return event.tid + "\n" + event.name; }

std::unordered_map<std::string, CustomEventGroup> group_custom_events(const std::vector<TraceEvent> & events) {
    using TimestampedEvent = std::pair<uint64_t, const TraceEvent *>;
    std::unordered_map<std::string, std::vector<TimestampedEvent>> grouped;
    for (const auto & event : events) {
        if (event.ph == 'X') grouped[custom_key(event)].push_back({ event.ts, &event });
    }

    std::unordered_map<std::string, CustomEventGroup> result;
    result.reserve(grouped.size());
    for (auto & [key, rows] : grouped) {
        std::ranges::sort(rows, {}, &TimestampedEvent::first);
        auto & group = result[key];
        group.timestamps.reserve(rows.size());
        group.events.reserve(rows.size());
        for (const auto & [timestamp, event] : rows) {
            group.timestamps.push_back(timestamp);
            group.events.push_back(event);
        }
        group.used.resize(rows.size(), false);
    }
    return result;
}

std::string function_argument(const TraceEvent & event, const std::string & key) {
    auto direct = event.arg("Function-Args." + key);
    if (!direct.empty()) return direct;
    const auto packed = event.arg("Function-Args");
    if (packed.empty()) return {};

    try {
        auto value = Json::parse(packed);
        if (value.is_string()) value = Json::parse(value.get<std::string>());
        if (!value.is_object() || !value.contains(key)) return {};
        const auto & field = value[key];
        if (field.is_string()) return field.get<std::string>();
        if (field.is_number_unsigned()) return std::to_string(field.get<uint64_t>());
        if (field.is_number_integer()) return std::to_string(field.get<int64_t>());
        if (field.is_number_float()) return std::to_string(field.get<double>());
        if (field.is_boolean()) return field.get<bool>() ? "true" : "false";
    }
    catch (const Json::exception &) {
        return {};
    }
    return {};
}

void inject_arguments(TraceEvent & profiler_event, const TraceEvent & custom_event) {
    const auto stream = function_argument(custom_event, "stream");
    if (!stream.empty()) profiler_event.set_arg("Raw Stream", stream);
    const auto event_id = function_argument(custom_event, "event");
    if (!event_id.empty()) profiler_event.set_arg("Event Id", event_id);
    profiler_event.merge_args_from(custom_event);
}

void join_by_timestamp_search(std::vector<TraceEvent> & profiler_events, const std::vector<TraceEvent> & custom_events,
                              const ManifestTraceInputOptions & options) {
    const auto pid = cann_pid(profiler_events);
    auto custom_groups = group_custom_events(custom_events);
    size_t candidates = 0;
    size_t matched = 0;

    for (auto & event : profiler_events) {
        if (event.ph != 'X' || event.pid != pid || event.name.empty()) continue;
        const auto group_entry = custom_groups.find(custom_key(event));
        if (group_entry == custom_groups.end()) continue;
        ++candidates;

        auto & group = group_entry->second;
        const auto insertion = std::upper_bound(group.timestamps.begin(), group.timestamps.end(), event.ts);
        const auto insertion_index = static_cast<size_t>(std::distance(group.timestamps.begin(), insertion));
        const auto begin = insertion_index > options.search_window ? insertion_index - options.search_window : 0;
        const auto end = std::min(group.timestamps.size(), insertion_index + options.search_window);

        size_t nearest_index = group.timestamps.size();
        uint64_t nearest_difference = std::numeric_limits<uint64_t>::max();
        for (size_t index = begin; index < end; ++index) {
            if (group.used[index]) continue;
            const auto timestamp = group.timestamps[index];
            const auto difference = event.ts >= timestamp ? event.ts - timestamp : timestamp - event.ts;
            if (static_cast<double>(difference) <= options.tolerance_us && difference < nearest_difference) {
                nearest_difference = difference;
                nearest_index = index;
            }
        }
        if (nearest_index < group.timestamps.size()) {
            group.used[nearest_index] = true;
            inject_arguments(event, *group.events[nearest_index]);
            ++matched;
        }
    }

    if (candidates != matched) {
        Logger::instance().warn() << "Manifest trace input matched " << matched << "/" << candidates << " LD_PRELOAD wrapper events.";
    }
}

} // namespace

void join_custom_trace(std::vector<TraceEvent> & profiler_events, std::vector<TraceEvent> custom_events, const ManifestTraceInputOptions & options) {
    join_by_timestamp_search(profiler_events, custom_events, options);

    const auto earliest = earliest_timestamp(profiler_events);
    const auto margin = core::truncate_to_u64(options.margin_us).value_or(0);
    const auto cutoff = earliest > margin ? earliest - margin : 0;
    std::erase_if(custom_events, [&](const TraceEvent & event) { return !is_standalone_event(event) || event.ts < cutoff; });
    const auto offset = profiler_events.size();
    for (size_t index = 0; index < custom_events.size(); ++index) custom_events[index].index = offset + index;
    profiler_events.insert(profiler_events.end(), std::make_move_iterator(custom_events.begin()), std::make_move_iterator(custom_events.end()));
}

void append_standalone_events(std::vector<TraceEvent> & target, std::vector<TraceEvent> source) {
    std::erase_if(source, [](const TraceEvent & event) { return !is_standalone_event(event); });
    const auto offset = target.size();
    for (size_t index = 0; index < source.size(); ++index) source[index].index = offset + index;
    target.insert(target.end(), std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
}

void retain_duration_events(std::vector<TraceEvent> & events) {
    std::erase_if(events, [](const TraceEvent & event) { return event.ph != 'X'; });
}

} // namespace markov::trace_graph::io::detail
