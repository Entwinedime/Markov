/**
 * @file
 * @brief Selects and loads logical trace inputs from `profile_manifest.json`.
 */
#include "markov/trace_graph/io/trace_manifest_input.hpp"
#include "markov/trace_graph/adapters/runtimes/ascend/event_semantics.hpp"

#include "markov/trace_graph/io/trace_channel_join.hpp"

#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/core/dag_builder_normalization.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/io/chrome_trace_io.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <ranges>
#include <regex>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace markov::trace_graph::io {

namespace {

using core::TraceEvent;
using core::TraceSourceChannel;
using Json = nlohmann::json;

constexpr std::string_view kWorkspacePrefix = "/workspace/trace-sim";
constexpr std::string_view kOptPrefix = "/opt/trace-sim";
// SGLang clips scheduler reservations, never actual generated output.
constexpr uint64_t kSglangDefaultGenerationReservationTokens = 4'096;

struct ManifestPaths {
    std::vector<std::string> torch;
    std::vector<std::string> ld_preload;
    std::vector<std::string> python_probe;
    std::unordered_map<std::string, Json> host_clocks;
    std::unordered_map<std::string, std::vector<std::string>> cann_runtime_traces;
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
    ManifestPaths paths{
        .torch = options.include_torch ? existing_paths(trace.value("torch_trace_files", Json::array()), "torch") : std::vector<std::string>{},
        .ld_preload =
            options.include_ld_preload ? existing_paths(trace.value("ld_preload_trace_files", Json::array()), "LD_PRELOAD") : std::vector<std::string>{},
        .python_probe =
            options.include_python_probe ? existing_paths(sidecar.value("python_probe_files", Json::array()), "Python probe") : std::vector<std::string>{},
    };
    for (const auto & entry : trace.value("torch_trace_files", Json::array())) {
        const auto path = map_repo_path(entry.at("path").get<std::string>());
        if (entry.contains("host_clock")) paths.host_clocks.emplace(path, entry.at("host_clock"));
        if (options.include_torch && entry.contains("cann_runtime_trace_files"))
            paths.cann_runtime_traces.emplace(path, existing_paths(entry.at("cann_runtime_trace_files"), "CANN runtime"));
    }
    return paths;
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

std::vector<TraceEvent> read_channel_trace(const std::string & path, const TraceReadOptions & options, TraceSourceChannel channel) {
    auto events = read_chrome_trace(path, options);
    for (auto & event : events) event.source_channel = channel;
    return events;
}

void append_trace_files(ManifestTraceInput & input, const std::vector<std::string> & paths, const TraceReadOptions & options, TraceSourceChannel channel) {
    for (const auto & path : paths) {
        auto events = read_channel_trace(path, options, channel);
        const auto offset = input.events.size();
        for (size_t index = 0; index < events.size(); ++index) events[index].index = offset + index;
        input.events.insert(input.events.end(), std::make_move_iterator(events.begin()), std::make_move_iterator(events.end()));
    }
}

bool token_dictionary_context_event(const TraceEvent & event) {
    if (event.source_channel != TraceSourceChannel::PythonProbe) return false;
    return event.has_arg_key_hint("token_dictionary") || event.has_arg_key_hint("token_dictionaries");
}

bool semantic_tail_context_event(const TraceEvent & event) { return event.source_channel == TraceSourceChannel::PythonProbe && event.has_arg_key_hint("fact"); }

bool semantic_prelude_context_event(const TraceEvent & event) {
    return event.source_channel == TraceSourceChannel::PythonProbe
           && (event.has_arg_key_hint("fact") || event.name == "runtime.hicache.host_load_check" || event.name == "runtime.hicache.decode_allocation"
               || (event.name == "runtime.phase.attention" && event.arg("descriptor_present") != "false")
               || (event.name == "runtime.hicache.layer_waits" && event.arg("consumer_index") == "-1" && event.arg("status") == "returned"));
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

void retain_completion_peers(std::vector<ManifestTraceInput> & inputs, uint64_t end_us) {
    using Lane = std::pair<std::string, std::string>;
    using CollectiveKey = std::tuple<std::string, std::string, uint64_t>;
    std::map<Lane, std::vector<TraceEvent *>> envelopes;
    std::map<CollectiveKey, std::vector<TraceEvent *>> rounds;
    for (auto & input : inputs) {
        for (auto & event : input.events) {
            if (event.source_channel != TraceSourceChannel::PythonProbe) continue;

            if ((event.name == "runtime.hicache.write_completion" || event.name == "runtime.hicache.load_completion") && event.arg("status") == "returned"
                && event.arg("blocking") == "false")
                envelopes[{ event.pid, event.tid }].push_back(&event);
            if (event.name == "runtime.cpu_collective" && !event.arg("group").empty() && !event.arg("members").empty()
                && !event.arg("collective_index").empty())
                rounds[{ event.arg("group"), event.arg("members"), event.arg_u64("collective_index") }].push_back(&event);
        }
    }
    for (auto & [lane, calls] : envelopes) std::ranges::sort(calls, {}, [](const auto * event) { return event->ts; });

    // Resolve each rank's outer check before any rank is cropped. A matching
    // collective identity, not a time margin, establishes the peer relation.
    for (auto & [identity, calls] : rounds) {
        for (auto *& call : calls) {
            const auto lane = envelopes.find({ call->pid, call->tid });
            TraceEvent * parent = nullptr;
            if (lane != envelopes.end()) {
                auto candidate = std::ranges::upper_bound(lane->second, call->ts, {}, [](const auto * event) { return event->ts; });
                if (candidate != lane->second.begin()) {
                    --candidate;
                    if (call->ts + call->dur <= (*candidate)->ts + (*candidate)->dur) parent = *candidate;
                }
            }
            call = parent;
        }
    }

    // One already-started check can have several communication groups. Close
    // only their observed peers; a subsequent polling round has a new identity.
    // Missing envelopes remain missing and are rejected by the normal observer.
    bool changed;
    do {
        changed = false;
        for (const auto & [identity, calls] : rounds) {
            const bool retained = std::ranges::any_of(calls, [&](const auto * call) {
                return call && ((call->ts <= end_us && call->ts + call->dur > end_us) || call->arg("formal_window_context") == "causal_tail");
            });
            if (!retained) continue;

            for (auto * call : calls) {
                if (!call || call->ts <= end_us || call->arg("formal_window_context") == "causal_tail") continue;

                call->set_arg("formal_window_context", "causal_tail");
                call->set_arg("counts_toward_e2e", "false");
                changed = true;
            }
        }
    } while (changed);
}

void retain_trace_window(ManifestTraceInput & input, const ManifestTraceInputOptions & options) {
    if (!options.window_start_us || !options.window_end_us) return;
    const auto start = *options.window_start_us;
    const auto end = *options.window_end_us;
    // Boundary children may survive through a device connection after their
    // native submission was clipped. Preserve the containing queue identity
    // now, before CPU-leaf selection and window filtering lose that relation.
    using Lane = std::pair<std::string, std::string>;
    using Time = unsigned __int128;
    const auto start_ns = [](const TraceEvent & event) -> Time { return Time(event.ts) * 1'000 + event.ts_submicro_ns; };
    const auto end_ns = [&](const TraceEvent & event) { return start_ns(event) + Time(event.dur) * 1'000 + event.dur_submicro_ns; };
    // Clipping a nested child must not promote its crossing Torch envelope
    // into an executable CPU leaf. Preserve that observed parent role, not the
    // clipped child's service; the envelope's uninstrumented tail remains gap.
    std::map<Lane, std::vector<size_t>> opening_cpu_parents;
    for (size_t i = 0; i < input.events.size(); ++i) {
        const auto & event = input.events[i];
        if (event.source_channel == TraceSourceChannel::Torch && event.cat == "cpu_op" && start_ns(event) < Time(start) * 1'000
            && end_ns(event) >= Time(start) * 1'000 && !core::dag_builder_detail::is_submit_anchor_event(event))
            opening_cpu_parents[{ event.pid, event.tid }].push_back(i);
    }
    for (const auto & event : input.events) {
        if (event.source_channel != TraceSourceChannel::Torch || (event.cat != "cpu_op" && event.cat != "enqueue") || !event.dur
            || end_ns(event) >= Time(start) * 1'000)
            continue;
        const auto lane = opening_cpu_parents.find({ event.pid, event.tid });
        if (lane == opening_cpu_parents.end()) continue;

        for (const auto i : lane->second) {
            auto & parent = input.events[i];
            if (start_ns(parent) <= start_ns(event) && end_ns(event) <= end_ns(parent)) parent.set_arg("formal_window_cpu_parent", "true");
        }
    }

    std::map<Lane, std::vector<size_t>> boundary_tasks;
    for (size_t i = 0; i < input.events.size(); ++i) {
        const auto & event = input.events[i];
        const bool crosses_opening = start_ns(event) < Time(start) * 1'000 && end_ns(event) >= Time(start) * 1'000;
        if (event.cat == "dequeue" && (crosses_opening || end_ns(event) > Time(end) * 1'000) && !event.arg("correlation_id").empty())
            boundary_tasks[{ event.pid, event.tid }].push_back(i);
    }
    for (auto & [lane, tasks] : boundary_tasks) std::ranges::sort(tasks, {}, [&](size_t i) { return start_ns(input.events[i]); });
    for (auto & event : input.events) {
        if ((event.ts >= start && event.ts <= end) || !event.arg("correlation_id").empty()) continue;
        const auto lane = boundary_tasks.find({ event.pid, event.tid });
        if (lane == boundary_tasks.end()) continue;
        auto parent = std::ranges::upper_bound(lane->second, start_ns(event), {}, [&](size_t i) { return start_ns(input.events[i]); });
        if (parent == lane->second.begin()) continue;
        const auto & task = input.events[*--parent];
        if (end_ns(event) <= end_ns(task)) event.set_arg("correlation_id", task.arg("correlation_id"));
    }
    std::unordered_set<std::string> in_window_connection_ids;
    std::unordered_set<std::string> in_window_correlation_ids;
    std::vector<TimeInterval> semantic_tail_intervals;
    std::vector<const TraceEvent *> semantic_tail_calls;
    std::vector<ThreadCallInterval> cross_boundary_semantic_calls;
    for (const auto & event : input.events) {
        const auto event_end = event.dur > std::numeric_limits<uint64_t>::max() - event.ts ? std::numeric_limits<uint64_t>::max() : event.ts + event.dur;
        if (event_end < start && token_dictionary_context_event(event)) input.context_events.push_back(event);
        if (event_end < start && semantic_prelude_context_event(event)) input.prelude_context_events.push_back(event);
        // A rank may finish its last forward/sample after the client's HTTP
        // response. Preserve the rest of that already-started batch as causal
        // context, without extending the measured HTTP window or adding a new
        // request. Otherwise its logits/sampling descriptors disappear even
        // though the same batch's forward marker is retained.
        const bool generation_batch = event.source_channel == TraceSourceChannel::PythonProbe && event.name == "runtime.hicache.layer_waits"
                                      && event.arg("status") == "returned" && (event.arg("phase") == "EXTEND" || event.arg("phase") == "DECODE");
        const bool completion_check = event.source_channel == TraceSourceChannel::PythonProbe && event.arg("status") == "returned"
                                      && (event.name == "runtime.hicache.write_completion" || event.name == "runtime.hicache.load_completion");
        // A started nonblocking check owns its MIN and acknowledgement tail,
        // even when those children begin after the client response. Retain the
        // observed call, not subsequent polling rounds or a time margin.
        if (event.source_channel == TraceSourceChannel::PythonProbe && (semantic_tail_context_event(event) || generation_batch || completion_check)
            && event.dur > 0 && (event.ts <= end || event.arg("formal_window_context") == "causal_tail") && event_end > end && !event.tid.empty()
            && event.tid != "-1") {
            cross_boundary_semantic_calls.push_back(ThreadCallInterval{
                .start_us = event.ts,
                .end_us = event_end,
                .tid = event.tid,
            });
            if (generation_batch || completion_check) semantic_tail_calls.push_back(&event);
        }
        if (event.ts > end && semantic_tail_context_event(event)) {
            input.tail_context_events.push_back(event);
            if (event.dur > 0) semantic_tail_intervals.emplace_back(event.ts, event_end);
            if (event.arg("phase") == "end" && Json::parse(event.arg("fact")).value("role", "") == "cache_lifecycle_commit")
                semantic_tail_calls.push_back(&event);
        }
        if (event_end < start || event.ts > end) continue;
        append_causality_identity(event, "connection_id", in_window_connection_ids);
        append_causality_identity(event, "correlation_id", in_window_correlation_ids);
    }
    // Device work submitted before the opening cut can execute inside the window.
    // Preserve its unique native submission as metadata before removing that CPU
    // work. It proves stream placement, not an extra executable submission.
    std::unordered_map<std::string, const TraceEvent *> pre_window_launches;
    for (const auto & event : input.events) {
        if (!core::dag_builder_detail::is_submit_anchor_event(event) || end_ns(event) >= Time(start) * 1'000) continue;
        const auto connection = event.arg("connection_id");
        if (connection.empty()) continue;

        const auto [found, inserted] = pre_window_launches.emplace(connection, &event);
        if (inserted || !found->second) continue;

        const auto & previous = *found->second;
        if (previous.pid != event.pid || previous.tid != event.tid) found->second = nullptr;
        else if (start_ns(previous) <= start_ns(event) && end_ns(event) <= end_ns(previous)) found->second = &event;
        else if (start_ns(event) > start_ns(previous) || end_ns(previous) > end_ns(event)) found->second = nullptr;
    }
    for (auto & event : input.events) {
        if (end_ns(event) < Time(start) * 1'000 || event.ts > end) continue;
        const auto found = pre_window_launches.find(event.arg("connection_id"));
        if (found == pre_window_launches.end() || !found->second || !core::dag_builder_detail::resolve_event_lane(event, false).is_device) continue;

        const auto & launch = *found->second;
        event.set_arg("pre_window_submit_pid", launch.pid);
        event.set_arg("pre_window_submit_tid", launch.tid);
        event.set_arg("pre_window_submit_start_us", std::to_string(launch.ts));
        event.set_arg("pre_window_submit_end_us", std::to_string(static_cast<uint64_t>(end_ns(launch) / 1'000)));
        if (event.arg("correlation_id").empty() && !launch.arg("correlation_id").empty()) event.set_arg("correlation_id", launch.arg("correlation_id"));
    }
    for (auto & event : input.events) {
        if (event.ts <= end) continue;
        // A retained lifecycle or batch still executes after the HTTP endpoint. Keep
        // its nested semantic boundaries too, without admitting new endpoints.
        if (event.source_channel == TraceSourceChannel::PythonProbe && event.cat == "runtime_diagnostic"
            && std::ranges::any_of(semantic_tail_calls, [&](const auto * call) {
                   return event.pid == call->pid && event.tid == call->tid && start_ns(event) >= start_ns(*call) && end_ns(event) <= end_ns(*call);
               }))
            mark_causal_tail(event, in_window_connection_ids, in_window_correlation_ids);
        if (event.source_channel != TraceSourceChannel::PythonProbe
            && (overlaps_tail_context(event, semantic_tail_intervals) || strictly_nested_in_cross_boundary_call(event, cross_boundary_semantic_calls)))
            mark_causal_tail(event, in_window_connection_ids, in_window_correlation_ids);
    }
    // Synchronous CPU collectives execute their Gloo work on another thread.
    // These traces have no submit/worker correlation id, so retain the native
    // worker only within the same-process, completed collective envelope.
    // The collective observer still rejects missing or nonunique workers.
    std::vector<const TraceEvent *> tail_collectives;
    for (const auto & event : input.events)
        if (event.source_channel == TraceSourceChannel::PythonProbe && event.name == "runtime.cpu_collective" && event.arg("status") == "returned"
            && event.arg("async_op") == "false" && event.ts >= start && end_ns(event) > Time(end) * 1'000
            && (event.ts <= end || event.arg("formal_window_context") == "causal_tail"))
            tail_collectives.push_back(&event);

    std::vector<ThreadCallInterval> collective_worker_calls;
    for (auto & event : input.events) {
        if (event.ts <= end || event.source_channel != TraceSourceChannel::Torch || event.cat != "cpu_op"
            || (event.name != "gloo:all_reduce" && event.name != "gloo:broadcast"))
            continue;

        const bool belongs_to_call = std::ranges::any_of(tail_collectives, [&](const auto * call) {
            const bool same_operation = (event.name == "gloo:all_reduce" && call->arg("operation") == "all_reduce")
                                        || (event.name == "gloo:broadcast" && call->arg("operation") == "broadcast");
            return same_operation && event.pid == call->pid && event.tid != call->tid && start_ns(event) >= start_ns(*call) && end_ns(event) <= end_ns(*call);
        });
        if (!belongs_to_call) continue;

        mark_causal_tail(event, in_window_connection_ids, in_window_correlation_ids);
        collective_worker_calls.push_back({ event.ts, event.ts + event.dur, event.tid });
    }
    for (auto & event : input.events)
        if (event.ts > end && event.source_channel != TraceSourceChannel::PythonProbe && strictly_nested_in_cross_boundary_call(event, collective_worker_calls))
            mark_causal_tail(event, in_window_connection_ids, in_window_correlation_ids);

    // A collective can return after its last CPU leaf but before the window
    // ends. Its return gap needs the next same-lane call as an observed boundary.
    // Retain that call (including nested leaves), not an arbitrary time margin.
    std::map<Lane, std::optional<size_t>> return_successors;
    for (const auto & event : input.events) {
        if (event.source_channel == TraceSourceChannel::PythonProbe && event.name == "runtime.cpu_collective" && event.ts >= start && event.ts <= end
            && event.dur <= end - event.ts)
            return_successors.try_emplace({ event.pid, event.tid }, std::nullopt);
    }
    for (size_t i = 0; i < input.events.size(); ++i) {
        const auto & event = input.events[i];
        if (event.ts <= end || event.source_channel != TraceSourceChannel::Torch || event.cat != "cpu_op" || event.has_arg_key_hint("streamId")
            || event.has_arg_key_hint("Physic Stream Id"))
            continue;
        const auto lane = return_successors.find({ event.pid, event.tid });
        if (lane == return_successors.end()) continue;
        auto & chosen = lane->second;
        if (!chosen || start_ns(event) < start_ns(input.events[*chosen])
            || (start_ns(event) == start_ns(input.events[*chosen]) && end_ns(event) > end_ns(input.events[*chosen])))
            chosen = i;
    }
    for (auto & event : input.events) {
        if (event.ts <= end || event.source_channel == TraceSourceChannel::PythonProbe) continue;
        const auto lane = return_successors.find({ event.pid, event.tid });
        if (lane == return_successors.end() || !lane->second) continue;
        const auto & boundary = input.events[*lane->second];
        if (start_ns(event) >= start_ns(boundary) && end_ns(event) <= end_ns(boundary))
            mark_causal_tail(event, in_window_connection_ids, in_window_correlation_ids);
    }
    // Storage state is applied after its MIN, in a separate probe. Keep the
    // first following application as evidence even when it falls past the cut.
    std::map<Lane, std::vector<size_t>> storage_boundaries;
    for (size_t i = 0; i < input.events.size(); ++i) {
        const auto & event = input.events[i];
        if (event.source_channel == TraceSourceChannel::PythonProbe
            && (event.name == "runtime.hicache.storage_drain" || (event.name == "runtime.cpu_collective" && event.arg("role") == "storage_control_drain")))
            storage_boundaries[{ event.pid, event.tid }].push_back(i);
    }
    for (auto & [lane, ids] : storage_boundaries) {
        std::ranges::stable_sort(ids, {}, [&](size_t i) { return start_ns(input.events[i]); });
        for (size_t i = 1; i < ids.size(); ++i) {
            const auto & before = input.events[ids[i - 1]];
            auto & after = input.events[ids[i]];
            if (before.name == "runtime.cpu_collective" && before.ts >= start && before.ts <= end && after.name == "runtime.hicache.storage_drain"
                && after.ts > end)
                mark_causal_tail(after, in_window_connection_ids, in_window_correlation_ids);
        }
    }
    // Boundary calls can also carry queue identities. Close those dependencies
    // along with the original causal tail, independently of file order.
    bool changed;
    do {
        changed = false;
        for (auto & event : input.events) {
            if (event.ts <= end || event.arg("formal_window_context") == "causal_tail") continue;
            const bool causal_tail = shares_causality_identity(event, "connection_id", in_window_connection_ids)
                                     || shares_causality_identity(event, "correlation_id", in_window_correlation_ids);
            if (!causal_tail) continue;
            mark_causal_tail(event, in_window_connection_ids, in_window_correlation_ids);
            changed = true;
        }
    } while (changed);
    // If the cut lies in a proven same-thread gap, retain the cut itself,
    // not the preceding call's execution. A collective starting before the
    // first retained CPU leaf otherwise loses its observed entry interval.
    std::map<Lane, bool> head_context;
    for (const auto & event : input.events) {
        if (event.source_channel == TraceSourceChannel::PythonProbe && event.name == "runtime.cpu_collective" && event.arg("status") == "returned"
            && event.ts >= start && event.ts <= end && event.dur <= end - event.ts)
            head_context.try_emplace({ event.pid, event.tid }, false);
    }
    for (const auto & event : input.events) {
        if (event.source_channel != TraceSourceChannel::Torch || event.cat != "cpu_op") continue;
        const auto found = head_context.find({ event.pid, event.tid });
        if (found != head_context.end() && end_ns(event) < Time(start) * 1'000) found->second = true;
    }
    // A diagnostic envelope can straddle the opening cut while its nested
    // collective already returned before it. Keep those descriptors together
    // so consumers can distinguish a partial-window call from missing probes.
    // They describe context only: the pre-window CPU/device work stays excluded.
    std::vector<const TraceEvent *> head_calls;
    for (auto & event : input.events) {
        if (event.source_channel == TraceSourceChannel::PythonProbe && event.cat == "runtime_diagnostic" && start_ns(event) < Time(start) * 1'000
            && end_ns(event) >= Time(start) * 1'000) {
            head_calls.push_back(&event);
            // The already-running batch is context, not a formal forward to
            // remodel. Its executable tail remains selected by the window.
            if (event.name == "runtime.hicache.layer_waits") event.set_arg("formal_window_context", "causal_head");
        }
    }

    // A forward already in progress at the cut owns its nested descriptors
    // and Torch marker too. Keep its executable tail in the source graph, but
    // do not demand a complete target replacement for this partial batch.
    for (auto & event : input.events) {
        const bool descriptor = event.source_channel == TraceSourceChannel::PythonProbe && event.cat == "runtime_diagnostic";
        const bool marker =
            event.source_channel == TraceSourceChannel::Torch && (event.name.starts_with("step[EXTEND") || event.name.starts_with("step[DECODE"));
        if (!descriptor && !marker) continue;

        if (std::ranges::any_of(head_calls, [&](const auto * call) {
                return call->name == "runtime.hicache.layer_waits" && event.pid == call->pid && event.tid == call->tid && start_ns(event) >= start_ns(*call)
                       && end_ns(event) <= end_ns(*call);
            }))
            event.set_arg("formal_window_context", "causal_head");
    }

    // Decide before erase_if moves records referenced by head_calls.
    std::unordered_set<size_t> head_descriptors;
    for (const auto & event : input.events)
        if (event.source_channel == TraceSourceChannel::PythonProbe && event.cat == "runtime_diagnostic" && end_ns(event) < Time(start) * 1'000
            && std::ranges::any_of(head_calls, [&](const auto * call) {
                   return event.pid == call->pid && event.tid == call->tid && start_ns(event) >= start_ns(*call) && end_ns(event) <= end_ns(*call);
               }))
            head_descriptors.insert(event.index);

    // Probe return records carry their entry timestamp, while payload fields
    // may live only in the corresponding zero-duration start record. Retain
    // that exact descriptor for crossing calls; it is not executable work.
    using FactCall = std::tuple<std::string, std::string, std::string, uint64_t, uint16_t>;
    std::set<FactCall> crossing_fact_calls;
    for (const auto & event : input.events)
        if (event.source_channel == TraceSourceChannel::PythonProbe && event.has_arg_key_hint("fact") && event.arg("phase") == "end"
            && start_ns(event) < Time(start) * 1'000 && end_ns(event) >= Time(start) * 1'000)
            crossing_fact_calls.emplace(event.pid, event.tid, event.arg("target_id"), event.ts, event.ts_submicro_ns);
    for (const auto & event : input.events)
        if (event.source_channel == TraceSourceChannel::PythonProbe && event.has_arg_key_hint("fact") && event.arg("phase") == "start"
            && crossing_fact_calls.contains({ event.pid, event.tid, event.arg("target_id"), event.ts, event.ts_submicro_ns }))
            head_descriptors.insert(event.index);

    // Worker jobs can already be queued at the opening cut. Their original
    // enqueue is context, not CPU service inside the window. Give those jobs
    // zero-service arrival points on a separate observation lane, preserving
    // their observed FIFO order ahead of newly submitted formal work.
    using QueueIdentity = std::pair<std::string, std::string>;
    std::map<QueueIdentity, const TraceEvent *> opening_enqueues;
    for (const auto & event : input.events) {
        if (event.cat != "enqueue" || end_ns(event) >= Time(start) * 1'000) continue;
        const auto correlation = event.arg("correlation_id");
        if (correlation.empty()) continue;

        const auto [found, inserted] = opening_enqueues.emplace(QueueIdentity{ event.pid, correlation }, &event);
        if (!inserted) found->second = nullptr;
    }
    struct OpeningTask {
        const TraceEvent * enqueue;
        Lane worker;
    };
    std::vector<OpeningTask> opening_tasks;
    std::map<Lane, uint64_t> opening_queue_times;
    for (const auto & event : input.events) {
        if (event.cat != "dequeue" || end_ns(event) < Time(start) * 1'000 || (event.ts > end && event.arg("formal_window_context") != "causal_tail")) continue;

        const auto found = opening_enqueues.find({ event.pid, event.arg("correlation_id") });
        if (found == opening_enqueues.end() || !found->second || found->second->tid == event.tid) continue;

        const Lane worker{ event.pid, event.tid };
        opening_tasks.push_back({ found->second, worker });
        const auto at = std::min(start, event.ts);
        const auto [queue, inserted] = opening_queue_times.emplace(worker, at);
        if (!inserted) queue->second = std::min(queue->second, at);
    }
    std::ranges::sort(opening_tasks, {}, [&](const OpeningTask & task) { return std::pair{ start_ns(*task.enqueue), task.enqueue->index }; });
    std::vector<TraceEvent> opening_arrivals;
    for (const auto & task : opening_tasks) {
        TraceEvent arrival;
        arrival.name = "formal_window.queued_task";
        arrival.cat = "enqueue";
        arrival.pid = task.worker.first;
        arrival.tid = "formal_window.queue:" + task.worker.second;
        arrival.ts = opening_queue_times.at(task.worker);
        arrival.source_channel = TraceSourceChannel::Synthetic;
        arrival.submit_anchor = true;
        arrival.set_arg("correlation_id", task.enqueue->arg("correlation_id"));
        arrival.set_arg("counts_toward_e2e", "false");
        arrival.set_arg("formal_window_context", "causal_head");
        arrival.set_arg("pre_window_enqueue_tid", task.enqueue->tid);
        arrival.set_arg("pre_window_enqueue_start_us", std::to_string(task.enqueue->ts));
        arrival.set_arg("pre_window_enqueue_end_us", std::to_string(static_cast<uint64_t>(end_ns(*task.enqueue) / 1'000)));
        opening_arrivals.push_back(std::move(arrival));
    }

    input.window_removed_record_count = std::erase_if(input.events, [&](const TraceEvent & event) {
        if (head_descriptors.contains(event.index)) return false;
        // Future bookkeeping facts can overlap a new polling check after the
        // HTTP cut. That overlap does not admit its native event wait: retain
        // waits only within an already-started semantic call (or its proven
        // peer). Otherwise a producer in the modeled window keeps an orphan
        // consumer from an unmodeled subsequent confirmation round.
        if (event.ts > end && core::synchronizes_recorded_event(event) && !strictly_nested_in_cross_boundary_call(event, cross_boundary_semantic_calls))
            return true;

        // Pre-window preparation is model context, not pre-window execution.
        // DagBuilder retains these envelopes only as non-executable metadata.
        if (event.source_channel == TraceSourceChannel::PythonProbe && event.cat == "runtime_diagnostic"
            && (event.name == "runtime.triton.prepare" || event.name == "runtime.triton.load") && event.ts <= end)
            return false;
        const auto event_end = event.dur > std::numeric_limits<uint64_t>::max() - event.ts ? std::numeric_limits<uint64_t>::max() : event.ts + event.dur;
        const bool retained_causal_tail = event.arg("formal_window_context") == "causal_tail";
        return event_end < start || (event.ts > end && !retained_causal_tail);
    });
    for (auto & arrival : opening_arrivals) {
        input.events.push_back(std::move(arrival));
        ++input.window_boundary_count;
    }

    for (const auto & [lane, has_predecessor] : head_context) {
        if (!has_predecessor) continue;
        // Eligibility against executable work is checked after CPU-leaf
        // normalization: a crossing parent is not necessarily retained work.
        TraceEvent boundary;
        boundary.name = "formal_window.cpu_begin";
        boundary.cat = "observed_boundary";
        boundary.ph = 'X';
        boundary.pid = lane.first;
        boundary.tid = lane.second;
        boundary.ts = start;
        boundary.source_channel = TraceSourceChannel::Synthetic;
        boundary.set_arg("counts_toward_e2e", "false");
        input.events.push_back(std::move(boundary));
        ++input.window_boundary_count;
    }
    for (size_t index = 0; index < input.events.size(); ++index) input.events[index].index = index;
}

void select_input_events(ManifestTraceInput & input, const ManifestTraceInputOptions & options) {
    // Native submission roles are needed during opening-cut retention, before
    // framework observations are routed by the source construction boundary.
    for (auto & event : input.events) adapters::ascend::classify_event(event);

    input.joined_record_count = input.events.size();
    detail::retain_duration_events(input.events);
    input.duration_record_count = input.events.size();
    retain_trace_window(input, options);
}

void normalize_profiler_clock(std::vector<TraceEvent> & events, const Json & clock) {
    if (!clock.contains("unix_origin_ns")) return;

    const auto offset = clock.at("unix_origin_ns").get<int64_t>() - clock.at("origin_ns").get<int64_t>();
    for (auto & event : events) {
        if (event.ph == 'M') continue;

        auto timestamp = core::checked_add_u64(core::checked_multiply_u64(event.ts, 1'000, "profiler timestamp overflow"),
                                               event.ts_submicro_ns,
                                               "profiler timestamp overflow");
        if (offset >= 0) timestamp = core::checked_add_u64(timestamp, static_cast<uint64_t>(offset), "aligned profiler timestamp overflow");
        else {
            const auto magnitude = static_cast<uint64_t>(-(offset + 1)) + 1;
            if (timestamp < magnitude) throw std::runtime_error("profiler clock maps a timestamp before the Unix epoch");
            timestamp -= magnitude;
        }
        event.ts = timestamp / 1'000;
        event.ts_submicro_ns = timestamp % 1'000;
    }
}

ManifestTraceInput load_state_only_group(const ManifestPaths & paths, const std::vector<std::string> & input_contracts,
                                         const ManifestTraceInputOptions & options) {
    ManifestTraceInput input;
    input.input_contracts = input_contracts;
    append_trace_files(input, paths.ld_preload, sidecar_read_options(options), TraceSourceChannel::LdPreload);
    append_trace_files(input, paths.python_probe, sidecar_read_options(options), TraceSourceChannel::PythonProbe);
    return input;
}

void normalize_wait_clock(std::vector<TraceEvent> & events, const Json & clock) {
    for (auto & event : events) {
        if (event.source_channel != TraceSourceChannel::PythonProbe || event.name != "runtime.hicache.layer_waits" || event.arg("wait_clock") != "npu_syscnt")
            continue;
        if (clock.value("clock", "") != "npu_syscnt") throw std::runtime_error("NPU layer-wait counters require the matching Torch trace host_clock");
        const auto origin_tick = clock.at("origin_tick").get<int64_t>();
        const bool unix_aligned = clock.contains("unix_origin_ns");
        const auto origin_ns = clock.at(unix_aligned ? "unix_origin_ns" : "origin_ns").get<int64_t>();
        const auto scale = clock.at("ns_per_tick").get<double>();
        if (!std::isfinite(scale) || scale <= 0) throw std::runtime_error("invalid profiler counter scale");
        auto intervals = Json::parse(event.arg("wait_intervals"));
        for (auto & interval : intervals) {
            for (const auto endpoint : { 1, 2 }) {
                // Subtract before scaling, as Torch does, to retain precision
                // when the epoch is large. Keep nanoseconds for ownership.
                const auto delta = interval.at(endpoint).get<int64_t>() - origin_tick;
                interval[endpoint] = origin_ns + static_cast<int64_t>(delta * scale);
            }
        }
        event.set_arg("wait_intervals", intervals.dump());
        event.set_arg("wait_clock", unix_aligned ? "aligned_unix_ns" : "profiler_ns");
    }
}

ManifestTraceInput load_logical_input(const ManifestPaths & paths, const std::vector<std::string> & input_contracts, size_t index,
                                      const ManifestTraceInputOptions & options) {
    const auto pid = pid_from_path(paths.torch[index]);
    const auto found = paths.host_clocks.find(paths.torch[index]);
    const auto clock = found == paths.host_clocks.end() ? Json::object() : found->second;
    ManifestTraceInput input;
    input.input_contracts = input_contracts;
    input.events = read_channel_trace(paths.torch[index], profiler_read_options(options), TraceSourceChannel::Torch);
    if (const auto native = paths.cann_runtime_traces.find(paths.torch[index]); native != paths.cann_runtime_traces.end()) {
        // These are the host roles pruned from Level0's display timeline.
        // Device records already exist in trace_view and must not be doubled.
        const TraceReadOptions runtime_options{
            .include_metadata = true,
            .threads = options.file_threads,
            .name_prefixes =
                std::vector<std::string>(adapters::ascend::level0_pruned_event_prefixes.begin(), adapters::ascend::level0_pruned_event_prefixes.end()),
        };
        append_trace_files(input, native->second, runtime_options, TraceSourceChannel::Torch);
    }
    // Join containment and formal-window selection must see one clock. Raw
    // trace files and their original manifest transform remain unchanged.
    normalize_profiler_clock(input.events, clock);
    const auto custom_path = select_by_pid(paths.ld_preload, pid, "LD_PRELOAD");
    if (!custom_path.empty()) {
        detail::join_custom_trace(input.events, read_channel_trace(custom_path, sidecar_read_options(options), TraceSourceChannel::LdPreload), options);
    }
    append_trace_files(input, select_sidecars(paths.python_probe, pid), sidecar_read_options(options), TraceSourceChannel::PythonProbe);
    normalize_wait_clock(input.events, clock);
    return input;
}

} // namespace

std::vector<ManifestTraceInput> load_trace_inputs_from_manifest(const std::string & manifest_path, const ManifestTraceInputOptions & options) {
    const auto manifest = load_manifest(manifest_path);
    const auto paths = selected_paths(manifest, options);
    const auto input_contracts = manifest_input_contracts(manifest);
    std::vector<ManifestTraceInput> inputs(paths.torch.size());
    if (paths.torch.empty()) {
        if (paths.ld_preload.empty() && paths.python_probe.empty()) {
            throw std::runtime_error("profile manifest has no usable trace inputs: " + manifest_path);
        }
        inputs.push_back(load_state_only_group(paths, input_contracts, options));
    }
    else {
        const size_t concurrency = std::max<size_t>(1, std::min(options.threads, paths.torch.size()));
        for (size_t begin = 0; begin < paths.torch.size(); begin += concurrency) {
            const auto end = std::min(paths.torch.size(), begin + concurrency);
            std::vector<std::future<ManifestTraceInput>> futures;
            futures.reserve(end - begin);
            for (size_t index = begin; index < end; ++index) {
                futures.push_back(std::async(std::launch::async, [&paths, &input_contracts, &options, index] {
                    return load_logical_input(paths, input_contracts, index, options);
                }));
            }
            // Join in manifest order, independently of worker completion order.
            for (size_t index = begin; index < end; ++index) inputs[index] = futures[index - begin].get();
        }
    }

    if (options.window_start_us && options.window_end_us) retain_completion_peers(inputs, *options.window_end_us);
    for (auto & input : inputs) select_input_events(input, options);
    return inputs;
}

ManifestClientInput load_client_requests_from_manifest(const std::string & manifest_path, bool include_preparation) {
    const auto manifest = load_manifest(manifest_path);
    const auto reports = manifest.value("bench", Json::object()).value("workload_report_files", Json::array());
    if (reports.size() != 1) return { "missing_or_multiple_workload_reports", {} };
    const auto paths = existing_paths(reports, "workload report");
    Json report;
    std::ifstream(paths.front()) >> report;
    const auto window = report.value("formal_window", Json::object());
    if (report.value("status", "") != "completed" || !window.is_object() || !window.contains("formal_begin_ms") || !window.contains("formal_end_ms")
        || !report.contains("requests"))
        return { "missing_completed_formal_window", {} };
    const auto timestamp = [](const Json & row, const char * key) {
        const auto value = row.at(key).get<double>() * 1000.0;
        if (!std::isfinite(value) || value < 0 || value >= static_cast<double>(std::numeric_limits<uint64_t>::max()))
            throw std::runtime_error("invalid client timestamp");
        return static_cast<uint64_t>(value);
    };
    const auto begin = timestamp(window, "formal_begin_ms");
    const auto end = timestamp(window, "formal_end_ms");
    uint64_t generation_clip = std::numeric_limits<uint64_t>::max();
    if (manifest.at("framework") == "sglang") {
        Json config;
        std::ifstream(map_repo_path(manifest.at("config_path").get<std::string>())) >> config;
        const auto environment = config.value("env", Json::object());
        const auto limit = environment.contains("SGLANG_CLIP_MAX_NEW_TOKENS_ESTIMATION")
                               ? core::parse_u64(environment.at("SGLANG_CLIP_MAX_NEW_TOKENS_ESTIMATION").get<std::string>())
                               : std::optional<uint64_t>{ kSglangDefaultGenerationReservationTokens };
        if (!limit) throw std::runtime_error("SGLang generation reservation limit must be a nonnegative token count");

        generation_clip = *limit;
    }

    ManifestClientInput result{ "ready", {} };
    std::vector<core::ClientRequestTiming> preparation;
    std::optional<uint64_t> preparation_idle_since_us;
    const auto request_timing = [&](const Json & row, uint64_t start, uint64_t finish) {
        std::optional<uint64_t> generation_limit;
        if (row.contains("output_token_limit")) generation_limit = row.at("output_token_limit").get<uint64_t>();
        else if (report.value("forced_token", Json::object()).value("mode", "") == "replay" && row.contains("forced_output_count")
                 && !row.contains("finish_reason")) {
            // Fixed-length replay declares its complete output budget. EOS
            // records must retain their explicit upper limit instead.
            generation_limit = row.at("forced_output_count").get<uint64_t>();
        }

        return core::ClientRequestTiming{ row.at("logical_request_id").get<std::string>(),
                                          start,
                                          finish,
                                          0,
                                          generation_limit ? std::optional{ std::min(*generation_limit, generation_clip) } : std::nullopt };
    };
    bool inside = false;
    uint64_t prelude_finish = 0;
    bool prelude_ordered = true;
    for (const auto & row : report.at("requests")) {
        if (!row.contains("start_time_ms") || !row.contains("end_time_ms")) {
            result.hicache_idle_since_us.reset();
            if (inside) return { "formal_step_without_timing", {} };
            continue;
        }
        const auto start = timestamp(row, "start_time_ms"), finish = timestamp(row, "end_time_ms");
        if (finish <= begin) {
            if (include_preparation && row.value("kind", "") == "request") {
                if (row.value("status", "") != "ok") return { "failed_preparation_request", {} };
                if (preparation.empty()) preparation_idle_since_us = result.hicache_idle_since_us;

                preparation.push_back(request_timing(row, start, finish));
            }
            prelude_ordered &= start >= prelude_finish && finish >= start && result.requests.empty();
            prelude_finish = std::max(prelude_finish, finish);
            if (row.value("kind", "") == "barrier" && row.value("scope", "") == "hicache_idle" && row.value("status", "") == "ok" && prelude_ordered) {
                result.hicache_idle_since_us = finish;
                preparation.clear();
            }
            else if (!prelude_ordered || row.value("kind", "") != "checkpoint" || row.value("status", "") != "ok") result.hicache_idle_since_us.reset();
            continue;
        }
        if (start >= end) continue;
        if (row.value("kind", "") != "request") return { "formal_control_step_not_modeled", {} };
        if (row.value("status", "") != "ok") return { "failed_formal_request", {} };
        result.requests.push_back(request_timing(row, start, finish));
        inside = finish < end;
    }
    std::ranges::sort(result.requests, {}, &core::ClientRequestTiming::start_us);
    if (result.requests.empty() || result.requests.front().start_us != begin || result.requests.back().end_us != end)
        return { "formal_request_envelope_mismatch", {} };

    if (!preparation.empty()) {
        std::ranges::sort(preparation, {}, &core::ClientRequestTiming::start_us);
        result.formal_begin_index = preparation.size();
        result.requests.insert(result.requests.begin(), preparation.begin(), preparation.end());
        result.hicache_idle_since_us = preparation_idle_since_us;
    }

    std::unordered_map<std::string, std::vector<uint64_t>> submits;
    TraceReadOptions options;
    options.auto_repair = true;
    const auto sidecars = existing_paths(manifest.value("sidecar", Json::object()).value("python_probe_files", Json::array()), "Python probe");
    for (const auto & path : sidecars) {
        for (const auto & event : read_chrome_trace(path, options)) {
            if (event.name != "runtime.request.tokenizer_submit") continue;
            for (const auto & id : Json::parse(event.arg("request_ids")))
                submits[id.get<std::string>()].push_back(core::checked_add_u64(event.ts, event.dur, "client submission end overflow"));
        }
    }
    for (auto & request : result.requests) {
        const auto & found = submits[request.request_id];
        if (found.size() != 1) {
            result.status = "missing_or_ambiguous_tokenizer_submit";
            return result;
        }
        request.frontend_end_us = found.front();
    }
    return result;
}

} // namespace markov::trace_graph::io
