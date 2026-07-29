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
    if (!value.is_array()) return {};
    std::vector<std::string> paths;
    for (const auto & item : value) {
        std::string raw;
        if (item.is_object()) {
            if (item.contains("exists") && item["exists"].is_boolean() && !item["exists"].get<bool>()) continue;
            if (item.contains("path") && item["path"].is_string()) raw = item["path"].get<std::string>();
        }
        else if (item.is_string()) raw = item.get<std::string>();
        if (raw.empty()) throw std::runtime_error("profile manifest contains an invalid " + std::string(channel) + " trace path entry");
        auto path = map_repo_path(std::move(raw));
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

std::string select_by_pid_or_index(const std::vector<std::string> & paths, const std::string & pid, size_t index) {
    if (!pid.empty()) {
        const auto matching = std::ranges::find_if(paths, [&](const auto & path) { return pid_from_path(path) == pid; });
        if (matching != paths.end()) return *matching;
    }
    return index < paths.size() ? paths[index] : std::string{};
}

std::vector<std::string> select_sidecars(const std::vector<std::string> & paths, const std::string & pid, size_t index) {
    if (!pid.empty()) {
        auto matching = paths | std::views::filter([&](const auto & path) { return pid_from_path(path) == pid; });
        std::vector<std::string> selected(matching.begin(), matching.end());
        if (!selected.empty()) return selected;
    }
    return index < paths.size() ? std::vector<std::string>{ paths[index] } : std::vector<std::string>{};
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

#ifdef DEBUG
std::string_view trace_channel_name(TraceSourceChannel channel) {
    switch (channel) {
    case TraceSourceChannel::Torch:
        return "torch";
    case TraceSourceChannel::LdPreload:
        return "ld_preload";
    case TraceSourceChannel::PythonProbe:
        return "python_probe";
    case TraceSourceChannel::Synthetic:
        return "synthetic";
    case TraceSourceChannel::Unknown:
        return "unknown";
    }
    return "unknown";
}
#endif

struct ChannelTraceRead {
    std::vector<TraceEvent> events;
#ifdef DEBUG
    TraceReadTimings timings;
#endif
};

ChannelTraceRead read_channel_trace(const std::string & path, const TraceReadOptions & options, TraceSourceChannel channel) {
    ChannelTraceRead result;
#ifdef DEBUG
    auto timed = read_chrome_trace_with_timings(path, options);
    result.events = std::move(timed.events);
    result.timings = std::move(timed.timings);
    result.timings.channel = trace_channel_name(channel);
#else
    result.events = read_chrome_trace(path, options);
#endif
    for (auto & event : result.events) event.source_channel = channel;
    return result;
}

void record_trace_read(ManifestTraceInput & input, ChannelTraceRead & read) {
#ifdef DEBUG
    input.trace_read_timings.push_back(std::move(read.timings));
#else
    (void)input;
    (void)read;
#endif
}

void append_trace_files(ManifestTraceInput & input, const std::vector<std::string> & paths, const TraceReadOptions & options, TraceSourceChannel channel) {
    for (const auto & path : paths) {
        auto read = read_channel_trace(path, options, channel);
        record_trace_read(input, read);
        const auto offset = input.events.size();
        for (size_t index = 0; index < read.events.size(); ++index) read.events[index].index = offset + index;
        input.events.insert(input.events.end(), std::make_move_iterator(read.events.begin()), std::make_move_iterator(read.events.end()));
    }
}

bool token_dictionary_context_event(const TraceEvent & event) {
    if (event.source_channel != TraceSourceChannel::PythonProbe) return false;
    return event.has_arg_key_hint("token_dictionary") || event.has_arg_key_hint("token_dictionaries");
}

bool semantic_tail_context_event(const TraceEvent & event) {
    return event.source_channel == TraceSourceChannel::PythonProbe && event.has_arg_key_hint("fact");
}

void retain_trace_window(ManifestTraceInput & input, const ManifestTraceInputOptions & options) {
    if (!options.window_start_us || !options.window_end_us) return;
    const auto start = *options.window_start_us;
    const auto end = *options.window_end_us;
    for (const auto & event : input.events) {
        const auto event_end = event.dur > std::numeric_limits<uint64_t>::max() - event.ts ? std::numeric_limits<uint64_t>::max() : event.ts + event.dur;
        if (event_end < start && token_dictionary_context_event(event)) input.context_events.push_back(event);
        if (event.ts > end && semantic_tail_context_event(event)) input.tail_context_events.push_back(event);
    }
    std::erase_if(input.events, [&](const TraceEvent & event) {
        const auto event_end = event.dur > std::numeric_limits<uint64_t>::max() - event.ts ? std::numeric_limits<uint64_t>::max() : event.ts + event.dur;
        return event_end < start || event.ts > end;
    });
    for (size_t index = 0; index < input.events.size(); ++index) input.events[index].index = index;
}

ManifestTraceInput load_torch_group(const std::string & torch_path, const std::string & custom_path, const std::vector<std::string> & sidecar_paths,
                                    const std::vector<std::string> & input_contracts, const ManifestTraceInputOptions & options) {
    ManifestTraceInput input;
    input.input_contracts = input_contracts;
    auto torch = read_channel_trace(torch_path, profiler_read_options(options), TraceSourceChannel::Torch);
    record_trace_read(input, torch);
    input.events = std::move(torch.events);
    if (!custom_path.empty()) {
        auto custom = read_channel_trace(custom_path, sidecar_read_options(options), TraceSourceChannel::LdPreload);
        record_trace_read(input, custom);
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
        record_trace_read(input, custom);
        detail::append_standalone_events(input.events, std::move(custom.events));
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
                            select_by_pid_or_index(paths.ld_preload, pid, index),
                            select_sidecars(paths.python_probe, pid, index),
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
