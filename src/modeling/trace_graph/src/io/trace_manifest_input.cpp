/**
 * @file
 * @brief Selects and loads logical trace inputs from `profile_manifest.json`.
 */
#include "markov/trace_graph/io/trace_manifest_input.hpp"

#include "trace_channel_join.hpp"

#include "markov/trace_graph/io/chrome_trace_io.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <ranges>
#include <regex>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace markov::trace_graph::io {

namespace {

using core::TraceEvent;
using core::TraceSourceChannel;
using Json = nlohmann::json;

constexpr std::string_view kWorkspacePrefix = "/workspace/trace-sim";
constexpr std::string_view kOptPrefix = "/opt/trace-sim";

struct ManifestPaths {
    std::vector<std::string> torch;
    std::vector<std::string> ld_preload;
    std::vector<std::string> python_probe;
};

std::vector<std::string> manifest_input_contracts(const Json & manifest) {
    const auto profiling = manifest.contains("profiling") && manifest["profiling"].is_object() ? manifest["profiling"] : Json::object();
    const auto consumers = profiling.value("python_consumers", Json::array());
    if (!consumers.is_array()) return {};
    std::vector<std::string> contracts;
    for (const auto & consumer : consumers) {
        if (consumer.is_string() && !consumer.get_ref<const std::string &>().empty()) contracts.push_back(consumer.get<std::string>());
    }
    std::ranges::sort(contracts);
    contracts.erase(std::unique(contracts.begin(), contracts.end()), contracts.end());
    return contracts;
}

Json load_manifest(const std::string & path) {
    std::ifstream input(path);
    if (!input.is_open()) throw std::runtime_error("Failed to open profile manifest: " + path);
    try {
        auto manifest = Json::parse(input);
        if (!manifest.is_object()) throw std::runtime_error("Profile manifest root must be a JSON object: " + path);
        return manifest;
    }
    catch (const Json::exception & error) {
        throw std::runtime_error("Failed to parse profile manifest '" + path + "': " + error.what());
    }
}

std::string map_repo_path(std::string path) {
    const auto root = std::filesystem::current_path();
    auto remap = [&](std::string_view prefix) -> std::string {
        if (path == prefix) return root.string();
        const std::string prefix_text(prefix);
        if (path.starts_with(prefix_text + "/")) return (root / path.substr(prefix_text.size() + 1)).string();
        return {};
    };
    if (auto mapped = remap(kWorkspacePrefix); !mapped.empty()) return mapped;
    if (auto mapped = remap(kOptPrefix); !mapped.empty()) return mapped;
    return path;
}

std::vector<std::string> existing_paths(const Json & value, std::string_view channel) {
    if (!value.is_array()) throw std::runtime_error("profile manifest contains an invalid " + std::string(channel) + " trace file list");
    std::vector<std::string> paths;
    for (const auto & item : value) {
        if (!item.is_object()) throw std::runtime_error("profile manifest contains a non-object " + std::string(channel) + " trace entry");
        if (item.contains("exists") && (!item["exists"].is_boolean() || !item["exists"].get<bool>())) {
            throw std::runtime_error("profile manifest declares an unavailable " + std::string(channel) + " trace entry");
        }
        if (!item.contains("path") || !item["path"].is_string()) {
            throw std::runtime_error("profile manifest contains an invalid " + std::string(channel) + " trace path entry");
        }
        const auto raw = item["path"].get<std::string>();
        if (raw.empty()) throw std::runtime_error("profile manifest contains an invalid " + std::string(channel) + " trace path entry");
        auto path = map_repo_path(raw);
        if (!std::filesystem::is_regular_file(path)) {
            throw std::runtime_error("profile manifest declares a missing " + std::string(channel) + " trace file: " + path);
        }
        paths.push_back(std::move(path));
    }
    std::ranges::sort(paths);
    return paths;
}

ManifestPaths selected_paths(const Json & manifest, const ManifestTraceInputOptions & options) {
    const auto trace = manifest.contains("trace") && manifest["trace"].is_object() ? manifest["trace"] : Json::object();
    const auto sidecar = manifest.contains("sidecar") && manifest["sidecar"].is_object() ? manifest["sidecar"] : Json::object();
    return ManifestPaths{
        .torch = options.include_torch ? existing_paths(trace.value("torch_trace_files", Json::array()), "torch") : std::vector<std::string>{},
        .ld_preload =
            options.include_ld_preload ? existing_paths(trace.value("ld_preload_trace_files", Json::array()), "LD_PRELOAD") : std::vector<std::string>{},
        .python_probe =
            options.include_python_probe ? existing_paths(sidecar.value("python_probe_files", Json::array()), "Python probe") : std::vector<std::string>{},
    };
}

std::string pid_from_path(const std::string & path) {
    static const std::regex pid_pattern("pid([0-9]+)");
    static const std::regex timestamp_pattern("_([0-9]+)_20[0-9]{11,}");
    std::smatch match;
    if (std::regex_search(path, match, pid_pattern) && match.size() > 1) return match[1].str();
    if (std::regex_search(path, match, timestamp_pattern) && match.size() > 1) return match[1].str();
    return {};
}

std::string select_by_pid(const std::vector<std::string> & paths, const std::string & pid, std::string_view channel) {
    if (paths.empty()) return {};
    if (pid.empty()) throw std::runtime_error("cannot associate " + std::string(channel) + " trace: torch path has no process id");
    const auto matching = std::ranges::find_if(paths, [&](const auto & path) { return pid_from_path(path) == pid; });
    if (matching == paths.end()) throw std::runtime_error("profile manifest has no " + std::string(channel) + " trace for torch process " + pid);
    return *matching;
}

std::vector<std::string> select_sidecars(const std::vector<std::string> & paths, const std::string & pid) {
    if (paths.empty()) return {};
    if (pid.empty()) throw std::runtime_error("cannot associate Python probe trace: torch path has no process id");
    auto matching = paths | std::views::filter([&](const auto & path) { return pid_from_path(path) == pid; });
    std::vector<std::string> selected(matching.begin(), matching.end());
    if (selected.empty()) throw std::runtime_error("profile manifest has no Python probe trace for torch process " + pid);
    return selected;
}

TraceReadOptions profiler_read_options(const ManifestTraceInputOptions & options) {
    return TraceReadOptions{
        .include_metadata = true,
        .threads = options.file_threads,
    };
}

TraceReadOptions sidecar_read_options(const ManifestTraceInputOptions & options) {
    return TraceReadOptions{
        .auto_repair = true,
        .threads = options.file_threads,
    };
}

struct ChannelTraceRead {
    std::vector<TraceEvent> events;
};

ChannelTraceRead read_channel_trace(const std::string & path, const TraceReadOptions & options, TraceSourceChannel channel) {
    ChannelTraceRead result;
    result.events = read_chrome_trace(path, options);
    for (auto & event : result.events) event.source_channel = channel;
    return result;
}

void append_trace_files(ManifestTraceInput & input, const std::vector<std::string> & paths, const TraceReadOptions & options, TraceSourceChannel channel) {
    for (const auto & path : paths) {
        auto read = read_channel_trace(path, options, channel);
        const auto offset = input.events.size();
        for (size_t index = 0; index < read.events.size(); ++index) read.events[index].index = offset + index;
        input.events.insert(input.events.end(), std::make_move_iterator(read.events.begin()), std::make_move_iterator(read.events.end()));
    }
}

bool token_dictionary_context_event(const TraceEvent & event) {
    if (event.source_channel != TraceSourceChannel::PythonProbe) return false;
    return event.has_arg_key_hint("token_dictionary") || event.has_arg_key_hint("token_dictionaries");
}

bool semantic_tail_context_event(const TraceEvent & event) { return event.source_channel == TraceSourceChannel::PythonProbe && event.has_arg_key_hint("fact"); }

bool semantic_prelude_context_event(const TraceEvent & event) {
    return event.source_channel == TraceSourceChannel::PythonProbe && event.has_arg_key_hint("fact");
}

void append_causality_identity(const TraceEvent & event, std::string_view key, std::unordered_set<std::string> & identities) {
    if (!event.has_arg_key_hint(key)) return;
    const auto value = event.find_arg(key);
    if (value && !value->empty()) identities.insert(*value);
}

bool shares_causality_identity(const TraceEvent & event, std::string_view key, const std::unordered_set<std::string> & identities) {
    if (!event.has_arg_key_hint(key)) return false;
    const auto value = event.find_arg(key);
    return value && identities.contains(*value);
}

using TimeInterval = std::pair<uint64_t, uint64_t>;

struct ThreadCallInterval {
    uint64_t start_us = 0;
    uint64_t end_us = 0;
    std::string tid;
};

bool overlaps_tail_context(const TraceEvent & event, const std::vector<TimeInterval> & intervals) {
    const auto event_end = event.dur > std::numeric_limits<uint64_t>::max() - event.ts ? std::numeric_limits<uint64_t>::max() : event.ts + event.dur;
    return std::ranges::any_of(intervals, [&](const auto & interval) { return event.ts <= interval.second && event_end >= interval.first; });
}

bool strictly_nested_in_cross_boundary_call(const TraceEvent & event, const std::vector<ThreadCallInterval> & intervals) {
    if (event.tid.empty() || event.tid == "-1") return false;
    const auto event_end = event.dur > std::numeric_limits<uint64_t>::max() - event.ts ? std::numeric_limits<uint64_t>::max() : event.ts + event.dur;
    return std::ranges::any_of(intervals, [&](const auto & interval) {
        return event.tid == interval.tid && event.ts >= interval.start_us && event_end <= interval.end_us;
    });
}

void mark_causal_tail(TraceEvent & event, std::unordered_set<std::string> & connection_ids, std::unordered_set<std::string> & correlation_ids) {
    event.set_arg("formal_window_context", "causal_tail");
    event.set_arg("counts_toward_e2e", "false");
    append_causality_identity(event, "connection_id", connection_ids);
    append_causality_identity(event, "correlation_id", correlation_ids);
}

void retain_trace_window(ManifestTraceInput & input, const ManifestTraceInputOptions & options) {
    if (!options.window_start_us || !options.window_end_us) return;
    const auto start = *options.window_start_us;
    const auto end = *options.window_end_us;
    std::unordered_set<std::string> in_window_connection_ids;
    std::unordered_set<std::string> in_window_correlation_ids;
    std::vector<TimeInterval> semantic_tail_intervals;
    std::vector<ThreadCallInterval> cross_boundary_semantic_calls;
    for (const auto & event : input.events) {
        const auto event_end = event.dur > std::numeric_limits<uint64_t>::max() - event.ts ? std::numeric_limits<uint64_t>::max() : event.ts + event.dur;
        if (event_end < start && token_dictionary_context_event(event)) input.context_events.push_back(event);
        if (event_end < start && semantic_prelude_context_event(event)) input.prelude_context_events.push_back(event);
        if (event.source_channel == TraceSourceChannel::PythonProbe && semantic_tail_context_event(event) && event.dur > 0 && event.ts <= end && event_end > end
            && !event.tid.empty() && event.tid != "-1") {
            cross_boundary_semantic_calls.push_back(ThreadCallInterval{
                .start_us = event.ts,
                .end_us = event_end,
                .tid = event.tid,
            });
        }
        if (event.ts > end && semantic_tail_context_event(event)) {
            input.tail_context_events.push_back(event);
            if (event.dur > 0) semantic_tail_intervals.emplace_back(event.ts, event_end);
        }
        if (event_end < start || event.ts > end) continue;
        append_causality_identity(event, "connection_id", in_window_connection_ids);
        append_causality_identity(event, "correlation_id", in_window_correlation_ids);
    }
    for (auto & event : input.events) {
        if (event.ts <= end) continue;
        if (event.source_channel != TraceSourceChannel::PythonProbe
            && (overlaps_tail_context(event, semantic_tail_intervals) || strictly_nested_in_cross_boundary_call(event, cross_boundary_semantic_calls)))
            mark_causal_tail(event, in_window_connection_ids, in_window_correlation_ids);
    }
    for (auto & event : input.events) {
        if (event.ts <= end || event.arg("formal_window_context") == "causal_tail") continue;
        const bool causal_tail = shares_causality_identity(event, "connection_id", in_window_connection_ids)
                                 || shares_causality_identity(event, "correlation_id", in_window_correlation_ids);
        if (!causal_tail) continue;
        mark_causal_tail(event, in_window_connection_ids, in_window_correlation_ids);
    }
    std::erase_if(input.events, [&](const TraceEvent & event) {
        const auto event_end = event.dur > std::numeric_limits<uint64_t>::max() - event.ts ? std::numeric_limits<uint64_t>::max() : event.ts + event.dur;
        const bool retained_causal_tail = event.arg("formal_window_context") == "causal_tail";
        return event_end < start || (event.ts > end && !retained_causal_tail);
    });
    for (size_t index = 0; index < input.events.size(); ++index) input.events[index].index = index;
}

ManifestTraceInput load_torch_group(const std::string & torch_path, const std::string & custom_path, const std::vector<std::string> & sidecar_paths,
                                    const std::vector<std::string> & input_contracts, const ManifestTraceInputOptions & options) {
    ManifestTraceInput input;
    input.input_contracts = input_contracts;
    auto torch = read_channel_trace(torch_path, profiler_read_options(options), TraceSourceChannel::Torch);
    input.events = std::move(torch.events);
    if (!custom_path.empty()) {
        auto custom = read_channel_trace(custom_path, sidecar_read_options(options), TraceSourceChannel::LdPreload);
        detail::join_custom_trace(input.events, std::move(custom.events), options);
    }
    append_trace_files(input, sidecar_paths, sidecar_read_options(options), TraceSourceChannel::PythonProbe);
    detail::retain_duration_events(input.events);
    retain_trace_window(input, options);
    return input;
}

ManifestTraceInput load_state_only_group(const ManifestPaths & paths, const std::vector<std::string> & input_contracts,
                                         const ManifestTraceInputOptions & options) {
    ManifestTraceInput input;
    input.input_contracts = input_contracts;
    for (const auto & path : paths.ld_preload) {
        auto custom = read_channel_trace(path, sidecar_read_options(options), TraceSourceChannel::LdPreload);
        const auto offset = input.events.size();
        for (size_t index = 0; index < custom.events.size(); ++index) custom.events[index].index = offset + index;
        input.events.insert(input.events.end(), std::make_move_iterator(custom.events.begin()), std::make_move_iterator(custom.events.end()));
    }
    append_trace_files(input, paths.python_probe, sidecar_read_options(options), TraceSourceChannel::PythonProbe);
    detail::retain_duration_events(input.events);
    retain_trace_window(input, options);
    return input;
}

ManifestTraceInput load_logical_input(const ManifestPaths & paths, const std::vector<std::string> & input_contracts, size_t index,
                                      const ManifestTraceInputOptions & options) {
    const auto pid = pid_from_path(paths.torch[index]);
    return load_torch_group(paths.torch[index],
                            select_by_pid(paths.ld_preload, pid, "LD_PRELOAD"),
                            select_sidecars(paths.python_probe, pid),
                            input_contracts,
                            options);
}

} // namespace

std::vector<ManifestTraceInput> load_trace_inputs_from_manifest(const std::string & manifest_path, const ManifestTraceInputOptions & options) {
    const auto manifest = load_manifest(manifest_path);
    const auto paths = selected_paths(manifest, options);
    const auto input_contracts = manifest_input_contracts(manifest);
    if (paths.torch.empty()) {
        if (paths.ld_preload.empty() && paths.python_probe.empty()) {
            throw std::runtime_error("profile manifest has no usable trace inputs: " + manifest_path);
        }
        return { load_state_only_group(paths, input_contracts, options) };
    }

    std::vector<ManifestTraceInput> inputs(paths.torch.size());
    const size_t concurrency = std::max<size_t>(1, std::min(options.threads, paths.torch.size()));
    if (concurrency == 1) {
        for (size_t index = 0; index < paths.torch.size(); ++index) { inputs[index] = load_logical_input(paths, input_contracts, index, options); }
        return inputs;
    }

    for (size_t begin = 0; begin < paths.torch.size(); begin += concurrency) {
        const auto end = std::min(paths.torch.size(), begin + concurrency);
        std::vector<std::future<std::pair<size_t, ManifestTraceInput>>> futures;
        futures.reserve(end - begin);
        for (size_t index = begin; index < end; ++index) {
            futures.push_back(std::async(std::launch::async, [&paths, &input_contracts, &options, index] {
                return std::make_pair(index, load_logical_input(paths, input_contracts, index, options));
            }));
        }
        for (auto & future : futures) {
            auto [index, input] = future.get();
            inputs[index] = std::move(input);
        }
    }
    return inputs;
}

} // namespace markov::trace_graph::io
