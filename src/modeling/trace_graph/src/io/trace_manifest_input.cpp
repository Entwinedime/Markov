/**
 * @file
 * @brief profile_manifest.json 的 C++ 内存合流实现。
 */
#include "markov/trace_graph/io/trace_manifest_input.hpp"

#include "markov/trace_graph/core/logger.hpp"
#include "markov/trace_graph/io/chrome_trace_io.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <ranges>
#include <regex>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace markov::trace_graph::io {

using core::Logger;
using core::TraceEvent;

namespace manifest_input_detail {

using Json = nlohmann::json;

constexpr std::string_view kWorkspacePrefix = "/workspace/trace-sim";
constexpr std::string_view kOptPrefix = "/opt/trace-sim";

std::filesystem::path repo_root_from_cwd() { return std::filesystem::current_path(); }

std::string map_repo_path(std::string path) {
    auto root = repo_root_from_cwd();
    auto remap = [&](std::string_view prefix) -> std::string {
        if (path == prefix) return root.string();
        const std::string prefix_text(prefix);
        if (path.starts_with(prefix_text + "/")) return (root / path.substr(prefix_text.size() + 1)).string();
        return "";
    };
    if (auto mapped = remap(kWorkspacePrefix); !mapped.empty()) return mapped;
    if (auto mapped = remap(kOptPrefix); !mapped.empty()) return mapped;
    return path;
}

Json load_json_file(const std::string & path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) throw std::runtime_error("Failed to open JSON file: " + path);
    try {
        return Json::parse(ifs);
    }
    catch (const std::exception & e) {
        throw std::runtime_error("Failed to parse JSON file '" + path + "': " + e.what());
    }
}

std::vector<std::string> existing_paths(const Json & value) {
    std::vector<std::string> paths;
    if (!value.is_array()) return paths;
    for (const auto & item : value) {
        std::string raw;
        if (item.is_object()) {
            if (item.contains("exists") && item["exists"].is_boolean() && !item["exists"].get<bool>()) continue;
            if (item.contains("path") && item["path"].is_string()) raw = item["path"].get<std::string>();
        }
        else if (item.is_string()) raw = item.get<std::string>();
        if (raw.empty()) continue;
        auto path = map_repo_path(raw);
        if (std::filesystem::is_regular_file(path)) paths.push_back(path);
    }
    std::ranges::sort(paths);
    return paths;
}

std::string pid_from_path(const std::string & path) {
    static const std::regex pid_re("pid([0-9]+)");
    static const std::regex timestamp_re("_([0-9]+)_20[0-9]{11,}");
    std::smatch match;
    if (std::regex_search(path, match, pid_re) && match.size() > 1) return match[1].str();
    if (std::regex_search(path, match, timestamp_re) && match.size() > 1) return match[1].str();
    return "";
}

std::string select_by_pid_or_index(const std::vector<std::string> & paths, const std::string & pid, size_t index) {
    if (!pid.empty()) {
        for (const auto & path : paths) {
            if (pid_from_path(path) == pid) return path;
        }
    }
    return index < paths.size() ? paths[index] : "";
}

std::vector<std::string> select_sidecars(const std::vector<std::string> & paths, const std::string & pid) {
    if (pid.empty()) return paths;
    std::vector<std::string> selected;
    for (const auto & path : paths) {
        if (pid_from_path(path) == pid) selected.push_back(path);
    }
    return selected.empty() ? paths : selected;
}

std::string lower_string(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool should_append_standalone_event(const TraceEvent & event) {
    if (event.name == "CPUInfer::submit" || event.name == "CPUInfer::sync") return true;
    if (event.name.starts_with("HiCache::") || event.name.starts_with("hicache_")) return true;
    auto domain = lower_string(event.arg("domain", event.cat));
    return domain == "hicache" || domain == "cache_io" || domain == "python_probe";
}

uint64_t earliest_timestamp(const std::vector<TraceEvent> & events) {
    uint64_t earliest = 0;
    bool found = false;
    for (const auto & event : events) {
        if (!found || event.ts < earliest) {
            earliest = event.ts;
            found = true;
        }
    }
    return found ? earliest : 0;
}

std::string cann_pid(const std::vector<TraceEvent> & events) {
    for (const auto & event : events) {
        if (event.ph == "M" && event.name == "process_name" && event.arg("name") == "CANN") return event.pid;
    }
    return "0";
}

struct CustomEventMapEntry {
    std::vector<uint64_t> timestamps;
    std::vector<std::unordered_map<std::string, std::string>> args;
};

std::string custom_key(const TraceEvent & event) { return event.tid + "\n" + event.name; }

std::unordered_map<std::string, CustomEventMapEntry> build_custom_event_map(const std::vector<TraceEvent> & custom_events) {
    std::unordered_map<std::string, std::vector<std::pair<uint64_t, std::unordered_map<std::string, std::string>>>> temp;
    for (const auto & event : custom_events) {
        if (event.ph != "X") continue;
        temp[custom_key(event)].push_back({ event.ts, event.args_map() });
    }
    std::unordered_map<std::string, CustomEventMapEntry> result;
    result.reserve(temp.size());
    for (auto & item : temp) {
        auto & rows = item.second;
        std::ranges::sort(rows, [](const auto & a, const auto & b) { return a.first < b.first; });
        auto & entry = result[item.first];
        entry.timestamps.reserve(rows.size());
        entry.args.reserve(rows.size());
        for (auto & row : rows) {
            entry.timestamps.push_back(row.first);
            entry.args.push_back(std::move(row.second));
        }
    }
    return result;
}

std::string arg_from_function_args(const std::unordered_map<std::string, std::string> & args, const std::string & key) {
    if (const auto it = args.find("Function-Args." + key); it != args.end()) return it->second;
    const auto it = args.find("Function-Args");
    if (it == args.end() || it->second.empty()) return "";
    try {
        auto value = Json::parse(it->second);
        if (value.is_string()) value = Json::parse(value.get<std::string>());
        if (value.is_object() && value.contains(key)) {
            const auto & field = value[key];
            if (field.is_string()) return field.get<std::string>();
            if (field.is_number_unsigned()) return std::to_string(field.get<uint64_t>());
            if (field.is_number_integer()) return std::to_string(field.get<int64_t>());
            if (field.is_number_float()) return std::to_string(field.get<double>());
            if (field.is_boolean()) return field.get<bool>() ? "true" : "false";
        }
    }
    catch (...) {
    }
    return "";
}

void inject_custom_args(TraceEvent & profiler_event, const std::unordered_map<std::string, std::string> & custom_args) {
    auto stream = arg_from_function_args(custom_args, "stream");
    if (!stream.empty()) profiler_event.set_arg("Raw Stream", stream);
    auto event_id = arg_from_function_args(custom_args, "event");
    if (!event_id.empty()) profiler_event.set_arg("Event Id", event_id);
    for (const auto & item : custom_args) profiler_event.set_arg(item.first, item.second);
}

void inject_search_matches(std::vector<TraceEvent> & profiler_events,
                           const std::vector<TraceEvent> & custom_events,
                           const ManifestTraceInputOptions & options) {
    auto pid = cann_pid(profiler_events);
    auto custom_map = build_custom_event_map(custom_events);
    std::unordered_set<std::string> used_custom_indices;
    size_t need_match = 0;
    size_t matched = 0;
    for (auto & event : profiler_events) {
        if (event.ph != "X" || event.pid != pid || event.name.empty()) continue;
        auto map_it = custom_map.find(custom_key(event));
        if (map_it == custom_map.end()) continue;
        ++need_match;
        const auto & entry = map_it->second;
        auto insert = std::upper_bound(entry.timestamps.begin(), entry.timestamps.end(), event.ts);
        auto insert_index = static_cast<size_t>(std::distance(entry.timestamps.begin(), insert));
        auto begin = insert_index > options.search_window ? insert_index - options.search_window : 0;
        auto end = std::min(entry.timestamps.size(), insert_index + options.search_window);
        std::vector<std::pair<double, size_t>> candidates;
        for (size_t i = begin; i < end; ++i) {
            auto diff = std::fabs(static_cast<double>(event.ts) - static_cast<double>(entry.timestamps[i]));
            if (diff <= options.tolerance_us) candidates.push_back({ diff, i });
        }
        std::ranges::sort(candidates, [](const auto & a, const auto & b) { return a.first < b.first; });
        for (const auto & candidate : candidates) {
            auto key = custom_key(event) + "\n" + std::to_string(candidate.second);
            if (used_custom_indices.contains(key)) continue;
            used_custom_indices.insert(std::move(key));
            inject_custom_args(event, entry.args[candidate.second]);
            ++matched;
            break;
        }
    }
    if (need_match != matched) {
        Logger::instance().warn() << "Manifest trace input matched " << matched << "/" << need_match << " LD_PRELOAD wrapper events.";
    }
}

void inject_sequential_matches(std::vector<TraceEvent> & profiler_events,
                               const std::vector<TraceEvent> & custom_events,
                               const ManifestTraceInputOptions & options) {
    auto pid = cann_pid(profiler_events);
    auto custom_map = build_custom_event_map(custom_events);
    auto earliest_profiler_ts = earliest_timestamp(profiler_events);
    size_t need_match = 0;
    size_t matched = 0;
    size_t count_mismatch_count = 0;
    size_t rejected_count = 0;

    std::unordered_map<std::string, std::vector<TraceEvent *>> profiler_groups;
    profiler_groups.reserve(custom_map.size());
    for (auto & event : profiler_events) {
        if (event.ph != "X" || event.pid != pid || event.name.empty()) continue;
        auto key = custom_key(event);
        if (!custom_map.contains(key)) continue;
        profiler_groups[key].push_back(&event);
    }

    const auto cutoff = static_cast<double>(earliest_profiler_ts) - options.margin_us;
    for (auto & item : profiler_groups) {
        auto custom_it = custom_map.find(item.first);
        if (custom_it == custom_map.end()) continue;
        auto & profiler_group = item.second;
        std::ranges::sort(profiler_group, [](const TraceEvent * a, const TraceEvent * b) {
            if (a->ts != b->ts) return a->ts < b->ts;
            return a->index < b->index;
        });

        const auto & entry = custom_it->second;
        std::vector<size_t> valid_custom_indices;
        valid_custom_indices.reserve(entry.timestamps.size());
        for (size_t index = 0; index < entry.timestamps.size(); ++index) {
            if (static_cast<double>(entry.timestamps[index]) >= cutoff) valid_custom_indices.push_back(index);
        }

        need_match += profiler_group.size();
        if (profiler_group.size() != valid_custom_indices.size()) {
            ++count_mismatch_count;
            continue;
        }

        for (size_t i = 0; i < profiler_group.size(); ++i) {
            auto custom_index = valid_custom_indices[i];
            auto diff = std::fabs(static_cast<double>(profiler_group[i]->ts) - static_cast<double>(entry.timestamps[custom_index]));
            if (diff > options.tolerance_us) {
                ++rejected_count;
                continue;
            }
            inject_custom_args(*profiler_group[i], entry.args[custom_index]);
            ++matched;
        }
    }

    if (count_mismatch_count > 0 || rejected_count > 0 || need_match != matched) {
        Logger::instance().warn() << "Manifest sequential trace input matched " << matched << "/" << need_match
                                  << " LD_PRELOAD wrapper events; count_mismatch=" << count_mismatch_count
                                  << " rejected=" << rejected_count << ".";
    }
}

TraceReadOptions profiler_read_options(const ManifestTraceInputOptions & options) {
    TraceReadOptions read;
    read.include_metadata = true;
    read.threads = std::max<size_t>(1, options.file_threads);
    return read;
}

TraceReadOptions side_read_options(const ManifestTraceInputOptions & options) {
    TraceReadOptions read;
    read.auto_repair = true;
    read.threads = std::max<size_t>(1, options.file_threads);
    return read;
}

void sort_events(std::vector<TraceEvent> & events) {
    std::erase_if(events, [](const TraceEvent & event) { return event.ph != "X"; });
    std::ranges::sort(events, [](const TraceEvent & a, const TraceEvent & b) {
        if (a.ts != b.ts) return a.ts < b.ts;
        if (a.pid != b.pid) return a.pid < b.pid;
        if (a.tid != b.tid) return a.tid < b.tid;
        return a.name < b.name;
    });
    for (size_t i = 0; i < events.size(); ++i) events[i].index = i;
}

ManifestTraceInput load_torch_group(const std::string & torch_path,
                                    const std::string & custom_path,
                                    const std::vector<std::string> & sidecar_paths,
                                    const ManifestTraceInputOptions & options) {
    ManifestTraceInput input;
    input.torch_path = torch_path;
    input.ld_preload_path = custom_path;
    input.sidecar_paths = sidecar_paths;
    input.source_paths.push_back(torch_path);

    input.events = read_chrome_trace(torch_path, profiler_read_options(options));
    if (!custom_path.empty()) {
        input.source_paths.push_back(custom_path);
        auto custom_events = read_chrome_trace(custom_path, side_read_options(options));
        if (options.mode == "search") inject_search_matches(input.events, custom_events, options);
        else if (options.mode == "sequential") inject_sequential_matches(input.events, custom_events, options);
        else throw std::runtime_error("C++ manifest trace input supports trace_merge mode=search or sequential");
        auto cutoff = static_cast<double>(earliest_timestamp(input.events)) - options.margin_us;
        for (auto & event : custom_events) {
            if (should_append_standalone_event(event) && static_cast<double>(event.ts) >= cutoff) input.events.push_back(std::move(event));
        }
    }
    for (const auto & sidecar_path : sidecar_paths) {
        input.source_paths.push_back(sidecar_path);
        auto sidecar_events = read_chrome_trace(sidecar_path, side_read_options(options));
        input.events.insert(input.events.end(), std::make_move_iterator(sidecar_events.begin()), std::make_move_iterator(sidecar_events.end()));
    }
    sort_events(input.events);
    return input;
}

ManifestTraceInput load_state_only_group(const std::vector<std::string> & ld_paths,
                                         const std::vector<std::string> & sidecar_paths,
                                         const ManifestTraceInputOptions & options) {
    ManifestTraceInput input;
    input.sidecar_paths = sidecar_paths;
    for (const auto & path : ld_paths) {
        input.source_paths.push_back(path);
        auto custom_events = read_chrome_trace(path, side_read_options(options));
        for (auto & event : custom_events) {
            if (should_append_standalone_event(event)) input.events.push_back(std::move(event));
        }
    }
    for (const auto & path : sidecar_paths) {
        input.source_paths.push_back(path);
        auto sidecar_events = read_chrome_trace(path, side_read_options(options));
        input.events.insert(input.events.end(), std::make_move_iterator(sidecar_events.begin()), std::make_move_iterator(sidecar_events.end()));
    }
    sort_events(input.events);
    return input;
}

} // namespace manifest_input_detail

using manifest_input_detail::existing_paths;
using manifest_input_detail::load_json_file;
using manifest_input_detail::load_state_only_group;
using manifest_input_detail::load_torch_group;
using manifest_input_detail::pid_from_path;
using manifest_input_detail::select_by_pid_or_index;
using manifest_input_detail::select_sidecars;

std::vector<ManifestTraceInput> load_trace_inputs_from_manifest(const std::string & manifest_path, const ManifestTraceInputOptions & options) {
    auto manifest = load_json_file(manifest_path);
    auto trace = manifest.contains("trace") && manifest["trace"].is_object() ? manifest["trace"] : nlohmann::json::object();
    auto sidecar = manifest.contains("sidecar") && manifest["sidecar"].is_object() ? manifest["sidecar"] : nlohmann::json::object();
    auto torch_paths = options.include_torch ? existing_paths(trace.value("torch_trace_files", nlohmann::json::array())) : std::vector<std::string>{};
    auto ld_paths = options.include_ld_preload ? existing_paths(trace.value("ld_preload_trace_files", nlohmann::json::array())) : std::vector<std::string>{};
    auto python_paths = options.include_python_probe ? existing_paths(sidecar.value("python_probe_files", nlohmann::json::array())) : std::vector<std::string>{};

    std::vector<ManifestTraceInput> inputs;
    if (torch_paths.empty() && (!ld_paths.empty() || !python_paths.empty())) {
        inputs.push_back(load_state_only_group(ld_paths, python_paths, options));
        return inputs;
    }

    inputs.resize(torch_paths.size());
    const size_t concurrency = std::max<size_t>(1, std::min(options.threads, torch_paths.size()));
    for (size_t base = 0; base < torch_paths.size(); base += concurrency) {
        std::vector<std::future<std::pair<size_t, ManifestTraceInput>>> futures;
        auto end = std::min(torch_paths.size(), base + concurrency);
        futures.reserve(end - base);
        for (size_t i = base; i < end; ++i) {
            futures.push_back(std::async(std::launch::async, [&, i] {
                auto pid = pid_from_path(torch_paths[i]);
                auto custom_path = select_by_pid_or_index(ld_paths, pid, i);
                auto sidecars = select_sidecars(python_paths, pid);
                return std::make_pair(i, load_torch_group(torch_paths[i], custom_path, sidecars, options));
            }));
        }
        for (auto & future : futures) {
            auto result = future.get();
            inputs[result.first] = std::move(result.second);
        }
    }
    if (inputs.empty()) throw std::runtime_error("profile manifest has no usable trace inputs: " + manifest_path);
    return inputs;
}

std::vector<std::string> trace_paths_from_manifest(const std::string & manifest_path) {
    auto manifest = load_json_file(manifest_path);
    auto trace = manifest.contains("trace") && manifest["trace"].is_object() ? manifest["trace"] : nlohmann::json::object();
    auto sidecar = manifest.contains("sidecar") && manifest["sidecar"].is_object() ? manifest["sidecar"] : nlohmann::json::object();
    std::set<std::string> paths;
    for (const auto & path : existing_paths(trace.value("torch_trace_files", nlohmann::json::array()))) paths.insert(path);
    for (const auto & path : existing_paths(trace.value("ld_preload_trace_files", nlohmann::json::array()))) paths.insert(path);
    for (const auto & path : existing_paths(sidecar.value("python_probe_files", nlohmann::json::array()))) paths.insert(path);
    return { paths.begin(), paths.end() };
}

} // namespace markov::trace_graph::io
