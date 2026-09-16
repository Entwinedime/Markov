/**
 * @file
 * @brief Request-bound source observations for SGLang prefill and decode phases.
 */
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"
#include "markov/trace_graph/modules/hicache/layer_waits.hpp"

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/fact.hpp"
#include "markov/trace_graph/modules/hicache/patch/io_operation_ledger.hpp"
#include "markov/trace_graph/modules/hicache/patch/source_dag_index.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <map>
#include <optional>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace markov::trace_graph::modules::hicache {

namespace {

using Json = nlohmann::json;
using core::TraceEvent;

struct Marker {
    int logical_input = 0;
    const TraceEvent * event = nullptr;
    uint64_t token_count = 0;
};

struct ExtendFact {
    const TraceEvent * event = nullptr;
    std::vector<std::string> request_ids;
    std::vector<uint64_t> token_counts;
    uint64_t batch_size = 0;
    uint64_t source_page_size = 0;
};

enum class PhaseKind : uint8_t { Prefill, Decode };

struct PhaseInterval {
    int logical_input = 0;
    std::string_view pid;
    uint64_t start_us = 0;
    uint64_t end_us = 0;
    size_t observation_index = 0;
    PhaseKind kind = PhaseKind::Prefill;
};

Json array_arg(const TraceEvent & event, std::string_view key) {
    if (!event.has_arg_key_hint(key)) return Json::array();
    const auto value = event.arg(key);
    if (value.empty()) return Json::array();
    try {
        auto parsed = Json::parse(value);
        return parsed.is_array() ? std::move(parsed) : Json::array();
    }
    catch (const Json::exception &) {
        return Json::array();
    }
}

std::vector<std::string> string_array_arg(const TraceEvent & event, std::string_view key) {
    std::vector<std::string> values;
    for (const auto & item : array_arg(event, key)) {
        if (item.is_string()) values.push_back(item.get<std::string>());
    }
    return values;
}

std::vector<uint64_t> u64_array_arg(const TraceEvent & event, std::string_view key) {
    std::vector<uint64_t> values;
    for (const auto & item : array_arg(event, key)) {
        if (item.is_number_unsigned()) values.push_back(item.get<uint64_t>());
        else if (item.is_number_integer()) {
            const auto value = item.get<int64_t>();
            if (value >= 0) values.push_back(static_cast<uint64_t>(value));
        }
    }
    return values;
}

std::optional<uint64_t> marker_value(std::string_view name, std::string_view key) {
    const auto begin = name.find(key);
    if (begin == std::string_view::npos) return std::nullopt;
    const auto value_begin = begin + key.size();
    auto value_end = value_begin;
    while (value_end < name.size() && name[value_end] >= '0' && name[value_end] <= '9') ++value_end;
    if (value_end == value_begin) return std::nullopt;
    uint64_t value = 0;
    const auto [position, error] = std::from_chars(name.data() + value_begin, name.data() + value_end, value);
    if (error != std::errc{} || position != name.data() + value_end) return std::nullopt;
    return value;
}

bool marker_order(const Marker & left, const Marker & right) {
    if (left.event->ts != right.event->ts) return left.event->ts < right.event->ts;
    return left.event->index < right.event->index;
}

bool fact_order(const ExtendFact & left, const ExtendFact & right) {
    if (left.event->ts != right.event->ts) return left.event->ts < right.event->ts;
    return left.event->index < right.event->index;
}

uint64_t sum_tokens(const std::vector<uint64_t> & values) {
    uint64_t total = 0;
    for (const auto value : values) total = core::checked_add_u64(total, value, "HiCache phase prompt token count exceeds uint64 range");
    return total;
}

bool is_device_control_anchor(std::string_view name) {
    return name == "EVENT_WAIT" || name == "EVENT_RECORD" || name == "NOTIFY_WAIT" || name == "NOTIFY_RECORD"
           || name == "MODEL_EXECUTE";
}

bool is_phase_cpu_control(const TraceEvent & event) {
    return event.name.starts_with("Enqueue@") || event.name == "Node@launch" || event.cat == "enqueue"
           || event.name.starts_with("AscendCL@aclrtLaunch") || event.name.starts_with("AscendCL@aclrtMemcpyAsync")
           || event.name == "AscendCL@aclrtRecordEvent" || event.name == "AscendCL@aclrtWaitEvent";
}

bool is_collective_node(std::string_view name) { return name.starts_with("hcom"); }

bool is_attention_node(std::string_view name) { return name.contains("Attention"); }

std::string phase_family_name(std::string_view name) {
    const auto instance_suffix = name.find("__");
    return std::string(name.substr(0, instance_suffix));
}

} // namespace

bool is_hicache_paged_attention(std::string_view name) {
    std::string normalized;
    normalized.reserve(name.size());
    for (const auto character : name) {
        if (character >= 'A' && character <= 'Z') normalized.push_back(static_cast<char>(character - 'A' + 'a'));
        else if ((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9')) normalized.push_back(character);
    }
    return normalized.contains("attention");
}

uint64_t hicache_paged_attention_duration(const std::map<std::string, HiCachePhaseCostFamily> & families) {
    uint64_t duration = 0;
    for (const auto & [name, family] : families) {
        if (!is_hicache_paged_attention(name)) continue;
        duration = core::checked_add_u64(duration, family.duration_us, "HiCache paged-attention duration exceeds uint64 range");
    }
    return duration;
}

HiCachePhaseObservationAudit observe_hicache_phases(const core::DagGraph & graph, const HiCacheLayerWaitObservation * layer_waits) {
    HiCachePhaseObservationAudit audit;
    HiCacheLayerWaitObservation local_waits;
    if (!layer_waits) {
        if (std::ranges::any_of(graph.runtime_observations(), [](const auto & event) { return event.name == "runtime.hicache.layer_waits"; }))
            local_waits = observe_hicache_layer_waits(patch::HiCacheSourceDagIndex(graph));
        layer_waits = &local_waits;
    }
    audit.layer_wait_status = layer_waits->status;
    audit.layer_wait_issues = layer_waits->issues;
    std::vector<PhaseInterval> phase_intervals;
    std::unordered_map<std::string, std::vector<ExtendFact>> facts_by_pid;
    for (const auto & event : graph.hicache_fact_events()) {
        try {
            const auto metadata = parse_hicache_fact_metadata(event);
            if (metadata.role != "cache_extend_input" || event.arg("phase") != "start") continue;
            ExtendFact fact{
                .event = &event,
                .request_ids = string_array_arg(event, "request_ids"),
                .token_counts = u64_array_arg(event, "token_counts"),
                .batch_size = event.arg_u64("batch_size", 0),
                .source_page_size = event.arg_u64("source_page_size", 0),
            };
            ++audit.cache_extend_fact_count;
            if (!fact.request_ids.empty()) ++audit.request_bound_fact_count;
            const auto expected_batch = fact.batch_size ? fact.batch_size : fact.request_ids.size();
            if (fact.request_ids.empty() || fact.request_ids.size() != expected_batch || fact.token_counts.size() != expected_batch) {
                ++audit.invalid_fact_count;
            }
            facts_by_pid[event.pid].push_back(std::move(fact));
        }
        catch (const std::exception &) {
            ++audit.invalid_fact_count;
        }
    }

    std::unordered_map<std::string, std::vector<Marker>> prefills_by_pid;
    std::unordered_map<std::string, std::vector<Marker>> decodes_by_pid;
    for (const auto & marker_event : graph.phase_marker_events()) {
        const auto & event = marker_event.event;
        if (event.name.starts_with("step[EXTEND")) {
            const auto tokens = marker_value(event.name, "toks=");
            if (!tokens) {
                ++audit.invalid_fact_count;
                continue;
            }
            prefills_by_pid[event.pid].push_back(Marker{ .logical_input = marker_event.gpu_id, .event = &event, .token_count = *tokens });
        }
        else if (event.name.starts_with("step[DECODE")) {
            decodes_by_pid[event.pid].push_back(Marker{ .logical_input = marker_event.gpu_id, .event = &event });
        }
    }

    std::unordered_set<const TraceEvent *> used_prefills;
    std::unordered_set<const TraceEvent *> used_decodes;
    for (auto & [pid, facts] : facts_by_pid) {
        std::ranges::sort(facts, fact_order);
        auto & prefills = prefills_by_pid[pid];
        auto & decodes = decodes_by_pid[pid];
        std::ranges::sort(prefills, marker_order);
        std::ranges::sort(decodes, marker_order);
        size_t prefill_cursor = 0;
        size_t decode_cursor = 0;
        for (size_t fact_index = 0; fact_index < facts.size(); ++fact_index) {
            const auto & fact = facts[fact_index];
            const auto next_fact_ts = fact_index + 1 < facts.size() ? facts[fact_index + 1].event->ts : std::numeric_limits<uint64_t>::max();
            while (prefill_cursor < prefills.size() && prefills[prefill_cursor].event->ts < fact.event->ts) ++prefill_cursor;
            if (prefill_cursor >= prefills.size() || prefills[prefill_cursor].event->ts >= next_fact_ts) continue;
            const auto & prefill = prefills[prefill_cursor++];
            used_prefills.insert(prefill.event);
            ++audit.paired_prefill_count;

            const auto prefill_end = core::checked_add_u64(prefill.event->ts, prefill.event->dur, "HiCache prefill marker end exceeds uint64 range");
            const auto next_prefill_ts = prefill_cursor < prefills.size() ? prefills[prefill_cursor].event->ts : next_fact_ts;
            const auto decode_end_boundary = std::min(next_fact_ts, next_prefill_ts);
            while (decode_cursor < decodes.size() && decodes[decode_cursor].event->ts < prefill_end) ++decode_cursor;
            uint64_t decode_iterations = 0;
            uint64_t decode_duration = 0;
            std::vector<const Marker *> owned_decodes;
            while (decode_cursor < decodes.size() && decodes[decode_cursor].event->ts < decode_end_boundary) {
                const auto & decode = decodes[decode_cursor++];
                used_decodes.insert(decode.event);
                decode_duration = core::checked_add_u64(decode_duration, decode.event->dur, "HiCache decode duration exceeds uint64 range");
                owned_decodes.push_back(&decode);
                ++decode_iterations;
            }
            audit.paired_decode_count += decode_iterations;
            ++audit.decode_iterations_histogram[decode_iterations];

            const auto prompt_tokens = sum_tokens(fact.token_counts);
            if (prefill.token_count > prompt_tokens) ++audit.token_range_error_count;
            const auto observation_index = audit.observations.size();
            audit.observations.push_back(HiCachePhaseObservation{
                .logical_input = prefill.logical_input,
                .pid = pid,
                .request_ids = fact.request_ids,
                .batch_size = fact.batch_size,
                .source_page_size = fact.source_page_size,
                .prompt_token_count = prompt_tokens,
                .prefill_token_count = prefill.token_count,
                .prefill_start_us = prefill.event->ts,
                .prefill_duration_us = prefill.event->dur,
                .decode_iteration_count = decode_iterations,
                .decode_duration_us = decode_duration,
            });
            phase_intervals.push_back(PhaseInterval{
                .logical_input = prefill.logical_input,
                .pid = pid,
                .start_us = prefill.event->ts,
                .end_us = core::checked_add_u64(prefill.event->ts, prefill.event->dur, "HiCache prefill interval exceeds uint64 range"),
                .observation_index = observation_index,
                .kind = PhaseKind::Prefill,
            });
            for (const auto * decode : owned_decodes) {
                phase_intervals.push_back(PhaseInterval{
                    .logical_input = decode->logical_input,
                    .pid = pid,
                    .start_us = decode->event->ts,
                    .end_us = core::checked_add_u64(decode->event->ts, decode->event->dur, "HiCache decode interval exceeds uint64 range"),
                    .observation_index = observation_index,
                    .kind = PhaseKind::Decode,
                });
            }
        }
    }

    const auto find_owner = [&](const core::DagNode & node, uint64_t anchor_us) {
        const PhaseInterval * owner = nullptr;
        for (const auto & interval : phase_intervals) {
            if (interval.logical_input != node.gpu_id || anchor_us < interval.start_us || anchor_us >= interval.end_us) continue;
            if (owner != nullptr) {
                ++audit.phase_owner_conflict_count;
                return static_cast<const PhaseInterval *>(nullptr);
            }
            owner = &interval;
        }
        return owner;
    };

    std::unordered_map<size_t, std::vector<size_t>> correlation_predecessors;
    for (const auto & edge : graph.edges()) {
        if (edge.active && edge.kind == core::DagEdgeKind::Correlation)
            correlation_predecessors[edge.dst].push_back(edge.src);
    }
    // CANN gives Node@launch a synthetic activity PID, whereas torch Dequeue
    // uses the real process PID. They still identify the same OS worker thread.
    // Retain containment only as an ownership link: adding a hard DAG edge from
    // the enclosing Dequeue's END to its nested launch would be incorrect.
    using FineTime = unsigned __int128;
    const auto fine_start = [](const TraceEvent & event) { return FineTime(event.ts) * 1000 + event.ts_submicro_ns; };
    const auto fine_end = [&](const TraceEvent & event) { return fine_start(event) + FineTime(event.dur) * 1000 + event.dur_submicro_ns; };
    std::map<std::pair<int, std::string>, std::vector<size_t>> worker_dequeues;
    for (const auto & node : graph.nodes()) {
        if (!node.active || !node.is_cpu || node.kind != core::DagNodeKind::TraceEvent) continue;
        const auto & event = graph.event_for_node(node.id);
        if (event.source_channel == core::TraceSourceChannel::Torch && event.name.starts_with("Dequeue@") && event.has_arg("correlation_id"))
            worker_dequeues[{ node.gpu_id, event.tid }].push_back(node.id);
    }
    for (auto & [key, lane] : worker_dequeues) {
        std::ranges::sort(lane, [&](size_t a, size_t b) { return fine_start(graph.event_for_node(a)) < fine_start(graph.event_for_node(b)); });
        // An overlapping queue is ambiguous, not permission to choose the latest.
        for (size_t i = 1; i < lane.size(); ++i) {
            if (fine_end(graph.event_for_node(lane[i - 1])) > fine_start(graph.event_for_node(lane[i]))) {
                lane.clear();
                break;
            }
        }
    }
    std::unordered_map<size_t, size_t> launch_dequeue;
    for (const auto & node : graph.nodes()) {
        if (!node.active || !node.is_cpu || node.kind != core::DagNodeKind::TraceEvent) continue;
        const auto & event = graph.event_for_node(node.id);
        if (event.source_channel != core::TraceSourceChannel::Torch || event.name != "Node@launch"
            || event.arg("Thread Id") != event.tid) continue;
        const auto found = worker_dequeues.find({ node.gpu_id, event.tid });
        if (found == worker_dequeues.end()) continue;
        const auto & lane = found->second;
        auto at = std::ranges::upper_bound(lane, fine_start(event), {}, [&](size_t id) { return fine_start(graph.event_for_node(id)); });
        if (at == lane.begin()) continue;
        const auto parent = *--at;
        if (fine_end(event) <= fine_end(graph.event_for_node(parent))) launch_dequeue.emplace(node.id, parent);
    }
    // Both device execution and the worker's launch may outlive the producer's
    // marker. Prefer the original Enqueue on the proven correlation chain, but
    // leave physical submit_ts and DAG dependencies untouched. An observed
    // producer outside all phases must not fall back to a coincident launch.
    const auto device_owner = [&](const core::DagNode & node) -> const PhaseInterval * {
        std::optional<const PhaseInterval *> producer;
        std::vector<size_t> pending{ node.id };
        std::unordered_set<size_t> visited{ node.id };
        while (!pending.empty()) {
            const auto id = pending.back();
            pending.pop_back();
            if (const auto parent = launch_dequeue.find(id); parent != launch_dequeue.end() && visited.insert(parent->second).second)
                pending.push_back(parent->second);
            const auto found = correlation_predecessors.find(id);
            if (found == correlation_predecessors.end()) continue;
            for (const auto predecessor_id : found->second) {
                if (!visited.insert(predecessor_id).second) continue;
                const auto & predecessor = graph.node(predecessor_id);
                if (!predecessor.active || predecessor.kind != core::DagNodeKind::TraceEvent || predecessor.gpu_id != node.gpu_id) continue;
                const auto & event = graph.event_for_node(predecessor_id);
                if (predecessor.is_cpu && event.source_channel == core::TraceSourceChannel::Torch
                    && (event.name.starts_with("Enqueue@") || event.cat == "enqueue")) {
                    const auto * owner = find_owner(predecessor, event.ts);
                    if (owner != nullptr && event.pid != owner->pid) owner = nullptr;
                    if (producer && *producer != owner) {
                        ++audit.phase_owner_conflict_count;
                        return nullptr;
                    }
                    producer = owner;
                }
                pending.push_back(predecessor_id);
            }
        }
        return producer ? *producer : find_owner(node, node.submit_ts);
    };
    std::map<size_t, const PhaseInterval *> device_owners;
    for (const auto & node : graph.nodes()) {
        if (!node.active || node.is_cpu || node.kind != core::DagNodeKind::TraceEvent || node.submit_ts == 0) continue;
        const auto * owner = device_owner(node);
        if (owner == nullptr) continue;
        device_owners.emplace(node.id, owner);
        const auto & event = graph.event_for_node(node.id);
        auto & observation = audit.observations[owner->observation_index];
        ++audit.phase_owned_device_node_count;
        auto & family = owner->kind == PhaseKind::Prefill ? audit.prefill_device_families[phase_family_name(event.name)]
                                                          : audit.decode_device_families[phase_family_name(event.name)];
        ++family.node_count;
        family.duration_us = core::checked_add_u64(family.duration_us, node.duration, "HiCache phase family duration exceeds uint64 range");
        if (owner->kind == PhaseKind::Prefill) {
            ++observation.prefill_device_node_count;
            observation.prefill_device_duration_us = core::checked_add_u64(
                observation.prefill_device_duration_us, node.duration, "HiCache prefill device duration exceeds uint64 range");
            if (!is_device_control_anchor(event.name)) {
                ++observation.prefill_compute_node_count;
                observation.prefill_compute_duration_us = core::checked_add_u64(
                    observation.prefill_compute_duration_us, node.duration, "HiCache prefill compute duration exceeds uint64 range");
                if (is_collective_node(event.name)) {
                    ++observation.prefill_collective_node_count;
                    observation.prefill_collective_node_ids.push_back(node.id);
                    observation.prefill_collective_duration_us = core::checked_add_u64(observation.prefill_collective_duration_us,
                                                                                       node.duration,
                                                                                       "HiCache prefill collective duration exceeds uint64 range");
                }
                else {
                    ++observation.prefill_kernel_node_count;
                    observation.prefill_kernel_node_ids.push_back(node.id);
                    if (is_attention_node(event.name)) {
                        ++observation.prefill_prefix_attention_node_count;
                        observation.prefill_prefix_attention_node_ids.push_back(node.id);
                        observation.prefill_prefix_attention_duration_us = core::checked_add_u64(
                            observation.prefill_prefix_attention_duration_us,
                            node.duration,
                            "HiCache prefill prefix-attention duration exceeds uint64 range");
                    }
                    else {
                        ++observation.prefill_common_kernel_node_count;
                        observation.prefill_common_kernel_node_ids.push_back(node.id);
                        observation.prefill_common_kernel_duration_us = core::checked_add_u64(
                            observation.prefill_common_kernel_duration_us,
                            node.duration,
                            "HiCache prefill common-kernel duration exceeds uint64 range");
                    }
                    auto & kernel_family = observation.prefill_kernel_families[phase_family_name(event.name)];
                    ++kernel_family.node_count;
                    kernel_family.duration_us = core::checked_add_u64(kernel_family.duration_us,
                                                                      node.duration,
                                                                      "HiCache prefill kernel family duration exceeds uint64 range");
                    observation.prefill_kernel_duration_us = core::checked_add_u64(
                        observation.prefill_kernel_duration_us, node.duration, "HiCache prefill kernel duration exceeds uint64 range");
                }
            }
        }
        else {
            ++observation.decode_device_node_count;
            observation.decode_device_duration_us = core::checked_add_u64(
                observation.decode_device_duration_us, node.duration, "HiCache decode device duration exceeds uint64 range");
            if (!is_device_control_anchor(event.name)) {
                ++observation.decode_compute_node_count;
                observation.decode_compute_duration_us = core::checked_add_u64(
                    observation.decode_compute_duration_us, node.duration, "HiCache decode compute duration exceeds uint64 range");
                if (is_collective_node(event.name)) {
                    ++observation.decode_collective_node_count;
                    observation.decode_collective_node_ids.push_back(node.id);
                    observation.decode_collective_duration_us = core::checked_add_u64(observation.decode_collective_duration_us,
                                                                                      node.duration,
                                                                                      "HiCache decode collective duration exceeds uint64 range");
                }
                else {
                    ++observation.decode_kernel_node_count;
                    observation.decode_kernel_node_ids.push_back(node.id);
                    auto & kernel_family = observation.decode_kernel_families[phase_family_name(event.name)];
                    ++kernel_family.node_count;
                    kernel_family.duration_us = core::checked_add_u64(kernel_family.duration_us,
                                                                      node.duration,
                                                                      "HiCache decode kernel family duration exceeds uint64 range");
                    observation.decode_kernel_duration_us = core::checked_add_u64(
                        observation.decode_kernel_duration_us, node.duration, "HiCache decode kernel duration exceeds uint64 range");
                }
            }
        }
    }

    // Only CPU nodes on a proven correlation chain into owned phase device work
    // are active phase control.  A Torch leaf merely occurring inside the broad
    // EXTEND/DECODE marker is not sufficient ownership evidence and remains in
    // the zero-cost dependency skeleton for gap-excluded replay.
    std::map<size_t, const PhaseInterval *> cpu_owners;
    for (const auto & node : graph.nodes()) {
        if (!node.active || !node.is_cpu || node.kind != core::DagNodeKind::TraceEvent) continue;
        if (layer_waits->cpu_node_ids.contains(node.id)) continue;
        const auto & event = graph.event_for_node(node.id);
        if (event.source_channel != core::TraceSourceChannel::Torch || !is_phase_cpu_control(event)) continue;
        const auto * owner = find_owner(node, event.ts);
        if (owner == nullptr) continue;
        const auto end_us = core::checked_add_u64(event.ts, event.dur, "HiCache phase CPU control end exceeds uint64 range");
        if (event.pid == owner->pid && end_us <= owner->end_us) cpu_owners.emplace(node.id, owner);
    }
    std::unordered_set<size_t> proven_cpu_owners;
    for (const auto & [device_id, owner] : device_owners) {
        std::vector<size_t> pending{ device_id };
        std::unordered_set<size_t> visited{ device_id };
        while (!pending.empty()) {
            const auto node_id = pending.back();
            pending.pop_back();
            const auto found = correlation_predecessors.find(node_id);
            if (found == correlation_predecessors.end()) continue;
            for (const auto predecessor_id : found->second) {
                if (!visited.insert(predecessor_id).second) continue;
                const auto & predecessor = graph.node(predecessor_id);
                if (!predecessor.active || predecessor.kind != core::DagNodeKind::TraceEvent) continue;
                if (!predecessor.is_cpu) {
                    pending.push_back(predecessor_id);
                    continue;
                }
                const auto & event = graph.event_for_node(predecessor_id);
                if (event.source_channel != core::TraceSourceChannel::Torch || predecessor.gpu_id != owner->logical_input
                    || event.pid != owner->pid || layer_waits->cpu_node_ids.contains(predecessor_id))
                    continue;
                // A delayed worker anchor follows its producer, not whichever
                // marker happens to overlap its launch. Conflicting device
                // consumers remain an error rather than picking one owner.
                if (proven_cpu_owners.insert(predecessor_id).second) cpu_owners[predecessor_id] = owner;
                else if (cpu_owners.at(predecessor_id) != owner)
                    ++audit.phase_owner_conflict_count;
            }
        }
    }
    for (const auto & [node_id, owner] : cpu_owners) {
        const auto & node = graph.node(node_id);
        auto & observation = audit.observations[owner->observation_index];
        ++audit.phase_owned_submit_cpu_node_count;
        if (owner->kind == PhaseKind::Prefill) {
            ++observation.prefill_submit_cpu_node_count;
            observation.prefill_submit_cpu_node_ids.push_back(node_id);
            observation.prefill_submit_cpu_duration_us = core::checked_add_u64(
                observation.prefill_submit_cpu_duration_us, node.duration, "HiCache prefill CPU duration exceeds uint64 range");
        }
        else {
            ++observation.decode_submit_cpu_node_count;
            observation.decode_submit_cpu_node_ids.push_back(node_id);
            observation.decode_submit_cpu_duration_us = core::checked_add_u64(
                observation.decode_submit_cpu_duration_us, node.duration, "HiCache decode CPU duration exceeds uint64 range");
        }
    }

    for (const auto & markers : prefills_by_pid | std::views::values) {
        audit.unmatched_prefill_marker_count += std::ranges::count_if(markers, [&](const auto & marker) { return !used_prefills.contains(marker.event); });
    }
    for (const auto & markers : decodes_by_pid | std::views::values) {
        audit.unmatched_decode_marker_count += std::ranges::count_if(markers, [&](const auto & marker) { return !used_decodes.contains(marker.event); });
    }
    std::ranges::sort(audit.observations, [](const auto & left, const auto & right) {
        if (left.prefill_start_us != right.prefill_start_us) return left.prefill_start_us < right.prefill_start_us;
        if (left.logical_input != right.logical_input) return left.logical_input < right.logical_input;
        return left.pid < right.pid;
    });
    const bool complete = audit.cache_extend_fact_count > 0 && audit.cache_extend_fact_count == audit.request_bound_fact_count
                          && audit.cache_extend_fact_count == audit.paired_prefill_count && audit.invalid_fact_count == 0
                          && audit.token_range_error_count == 0 && audit.unmatched_prefill_marker_count == 0
                          && audit.unmatched_decode_marker_count == 0 && audit.phase_owner_conflict_count == 0;
    audit.status = complete ? "ready" : "not_ready";
    return audit;
}

patch::HiCacheIoOperationLedger mark_observed_hicache_scope(core::DagGraph & graph) {
    graph.clear_scope_ownership();
    const auto source = patch::HiCacheSourceDagIndex(graph);
    const auto layer_waits = observe_hicache_layer_waits(source);
    const auto phases = observe_hicache_phases(graph, &layer_waits);
    const auto own_nodes = [&](const std::vector<size_t> & nodes) {
        for (const auto node_id : nodes) graph.set_scope_node_owned(node_id);
    };
    for (const auto & phase : phases.observations) {
        own_nodes(phase.prefill_common_kernel_node_ids);
        own_nodes(phase.prefill_prefix_attention_node_ids);
        own_nodes(phase.prefill_collective_node_ids);
        own_nodes(phase.prefill_submit_cpu_node_ids);
        own_nodes(phase.decode_kernel_node_ids);
        own_nodes(phase.decode_collective_node_ids);
        own_nodes(phase.decode_submit_cpu_node_ids);
    }

    auto operations = patch::build_hicache_io_operation_ledger(source);
    std::map<size_t, std::vector<std::pair<uint64_t, uint64_t>>> gap_intervals;
    const auto own_gaps = [&](const std::vector<patch::HiCacheCpuGapSlice> & slices) {
        for (const auto & slice : slices) {
            if (slice.owned_end_us > slice.owned_start_us)
                gap_intervals[slice.owner_node_id].emplace_back(slice.owned_start_us, slice.owned_end_us);
        }
    };
    for (const auto node_id : layer_waits.cpu_node_ids) graph.set_scope_node_owned(node_id);
    for (const auto node_id : layer_waits.device_wait_node_ids) graph.set_scope_node_owned(node_id);
    for (const auto & call : layer_waits.calls) if (call.issue.empty()) own_gaps(call.cpu.owned_gap_slices);
    for (const auto & operation : operations.records) {
        own_nodes(operation.runtime_node_ids);
        own_nodes(operation.admission_explicit_node_ids);
        own_nodes(operation.terminal_control_node_ids);
        own_nodes(operation.device_transfer_node_ids);
        own_nodes(operation.device_completion_node_ids);
        own_nodes(operation.readiness_join_node_ids);
        own_nodes(operation.completion_wait_owned_node_ids);
        own_gaps(operation.cpu_gap_slices);
        // The operation ledger deliberately separates explicit admission children
        // from Python wrapper self-time and idle slices.  Only the former is active
        // Direct control, together with the explicit terminal-check children;
        // wrapper/probe overhead remains residual Gap.
    }
    for (auto & [node_id, intervals] : gap_intervals) {
        std::ranges::sort(intervals);
        uint64_t total = 0;
        uint64_t begin = 0;
        uint64_t end = 0;
        bool active = false;
        for (const auto & interval : intervals) {
            if (!active || interval.first > end) {
                if (active) total = core::checked_add_u64(total, end - begin, "HiCache scope gap duration exceeds uint64 range");
                begin = interval.first;
                end = interval.second;
                active = true;
            }
            else end = std::max(end, interval.second);
        }
        if (active) total = core::checked_add_u64(total, end - begin, "HiCache scope gap duration exceeds uint64 range");
        graph.add_scope_gap_duration(node_id, total);
    }
    return operations;
}

} // namespace markov::trace_graph::modules::hicache
