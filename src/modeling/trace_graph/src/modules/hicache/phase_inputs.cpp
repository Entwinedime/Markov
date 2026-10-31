#include "modules/hicache/phase_observation.hpp"

#include "core/source_observations.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/fact.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#ifdef DEBUG
#include <cassert>
#endif
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

using FineTime = unsigned __int128;
FineTime fine_start(const TraceEvent & event) { return FineTime(event.ts) * 1'000 + event.ts_submicro_ns; }
FineTime fine_end(const TraceEvent & event) { return fine_start(event) + FineTime(event.dur) * 1'000 + event.dur_submicro_ns; }

/** Index only phase/batch envelopes, never the millions of executable events. */
class EnvelopeIndex {
    struct Entry {
        size_t id;
        FineTime begin, end;
    };
    struct Lane {
        std::vector<Entry> entries;
        std::vector<FineTime> prefix_end;
    };
    std::map<std::pair<std::string_view, std::string_view>, Lane> lanes_;

public:
    void add(std::string_view pid, std::string_view tid, size_t id, FineTime begin, FineTime end) {
        lanes_[{ pid, tid }].entries.push_back({ id, begin, end });
    }

    void finish() {
        for (auto & [thread, lane] : lanes_) {
            std::ranges::stable_sort(lane.entries, {}, &Entry::begin);
            FineTime end = 0;
            for (const auto & entry : lane.entries) {
                end = std::max(end, entry.end);
                lane.prefix_end.push_back(end);
            }
        }
    }

    std::vector<size_t> containing(std::string_view pid, std::string_view tid, FineTime begin, FineTime end) const {
        const auto found = lanes_.find({ pid, tid });
        if (found == lanes_.end()) return {};

        const auto & lane = found->second;
        auto at = std::ranges::upper_bound(lane.entries, begin, {}, &Entry::begin);
        std::vector<size_t> matches;
        while (at != lane.entries.begin()) {
            const auto position = static_cast<size_t>(--at - lane.entries.begin());
            if (lane.prefix_end[position] < end) break;
            if (at->end >= end) matches.push_back(at->id);
        }
#ifdef DEBUG
        std::vector<size_t> reference;
        for (const auto & entry : lane.entries)
            if (entry.begin <= begin && entry.end >= end) reference.push_back(entry.id);
        auto actual = matches;
        std::ranges::sort(reference);
        std::ranges::sort(actual);
        assert(actual == reference && "Envelope index must preserve every containing owner");
#endif
        return matches;
    }
};

} // namespace

HiCachePhaseInputs observe_hicache_phase_inputs(const core::SourceObservations & source) {
    HiCachePhaseInputs result;
    auto & audit = result.audit;
    auto & phase_intervals = result.intervals;
    std::unordered_map<std::string, std::vector<ExtendFact>> facts_by_pid;
    for (const auto & event : source.hicache_fact_events) {
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
            if (fact.request_ids.empty() || fact.request_ids.size() != expected_batch || fact.token_counts.size() != expected_batch
                || fact.source_page_size == 0) {
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
    for (const auto & marker_event : source.phase_marker_events) {
        const auto & event = marker_event.event;
        if (event.arg("formal_window_context") == "causal_head") {
            ++audit.opening_context_marker_count;
            continue;
        }

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
                .batch_size = fact.batch_size ? fact.batch_size : fact.request_ids.size(),
                .source_page_size = fact.source_page_size,
                .prompt_token_count = prompt_tokens,
                .prefill_token_count = prefill.token_count,
                .prefill_start_us = prefill.event->ts,
                .prefill_duration_us = prefill.event->dur,
                .decode_iteration_count = decode_iterations,
                .decode_duration_us = decode_duration,
            });
            phase_intervals.push_back(HiCachePhaseInterval{
                .envelope = prefill.event,
                .logical_input = prefill.logical_input,
                .pid = prefill.event->pid,
                .tid = prefill.event->tid,
                .start_us = prefill.event->ts,
                .end_us = core::checked_add_u64(prefill.event->ts, prefill.event->dur, "HiCache prefill interval exceeds uint64 range"),
                .observation_index = observation_index,
                .kind = HiCachePhaseKind::Prefill,
            });
            uint64_t iteration = 0;
            for (const auto * decode : owned_decodes) {
                phase_intervals.push_back(HiCachePhaseInterval{
                    .envelope = decode->event,
                    .logical_input = decode->logical_input,
                    .pid = decode->event->pid,
                    .tid = decode->event->tid,
                    .start_us = decode->event->ts,
                    .end_us = core::checked_add_u64(decode->event->ts, decode->event->dur, "HiCache decode interval exceeds uint64 range"),
                    .observation_index = observation_index,
                    .kind = HiCachePhaseKind::Decode,
                    .decode_iteration = iteration++,
                });
            }
        }
    }

    struct Batch {
        std::vector<std::string> requests;
        std::string phase;
        std::vector<size_t> forwards;
    };
    std::vector<Batch> batches;
    EnvelopeIndex batch_index;
    for (const auto & event : source.runtime_observations) {
        if (event.name != "runtime.hicache.layer_waits" || event.arg("status") != "returned") continue;
        const auto end = core::checked_add_u64(event.ts, event.dur, "Batch interval end overflow");
        batch_index.add(event.pid, event.tid, batches.size(), event.ts, end);
        batches.push_back({ string_array_arg(event, "request_ids"), event.arg("phase"), {} });
    }
    batch_index.finish();
    for (size_t i = 0; i < phase_intervals.size(); ++i) {
        const auto & interval = phase_intervals[i];
        for (const auto b : batch_index.containing(interval.pid, interval.tid, interval.start_us, interval.end_us)) {
            auto & batch = batches[b];
            if (batch.requests == audit.observations[interval.observation_index].request_ids
                && batch.phase == (interval.kind == HiCachePhaseKind::Prefill ? "EXTEND" : "DECODE"))
                batch.forwards.push_back(i);
        }
    }

    // Sampling follows the forward but belongs only to a unique containing request batch.
    std::vector<HiCachePhaseInterval> sampling_intervals;
    for (const auto & sample : source.runtime_observations) {
        if (sample.name != "runtime.phase.greedy_sampling" || sample.arg("status") != "returned") continue;
        const auto end = core::checked_add_u64(sample.ts, sample.dur, "Sampling interval end overflow");
        const HiCachePhaseInterval * owner = nullptr;
        size_t matches = 0;
        for (const auto b : batch_index.containing(sample.pid, sample.tid, sample.ts, end))
            for (const auto i : batches[b].forwards) {
                owner = &phase_intervals[i];
                ++matches;
            }
        if (matches != 1 || sample.ts < owner->end_us) continue;

        auto sampling = *owner;
        sampling.envelope = &sample;
        sampling.start_us = sample.ts;
        sampling.end_us = end;
        sampling_intervals.push_back(sampling);
    }
    phase_intervals.insert(phase_intervals.end(), sampling_intervals.begin(), sampling_intervals.end());

    // Retain nanosecond containment for call identity, unlike the batch microsecond join above.
    EnvelopeIndex phase_index;
    for (size_t i = 0; i < phase_intervals.size(); ++i) {
        const auto & interval = phase_intervals[i];
        phase_index.add(interval.pid, interval.tid, i, fine_start(*interval.envelope), fine_end(*interval.envelope));
    }
    phase_index.finish();
    for (size_t i = 0; i < source.runtime_observations.size(); ++i) {
        const auto & call = source.runtime_observations[i];
        if (!call.name.starts_with("runtime.phase.") || call.arg("status") != "returned" || call.arg("formal_window_context") == "causal_head") continue;
        const auto owners = phase_index.containing(call.pid, call.tid, fine_start(call), fine_end(call));
        if (owners.size() > 1) ++audit.phase_owner_conflict_count;
        if (owners.size() != 1) continue;

        const auto & owner = phase_intervals[owners.front()];
        auto & observation = audit.observations[owner.observation_index];
        if (owner.kind == HiCachePhaseKind::Prefill) observation.prefill_call_observations.push_back(i);
        else observation.decode_call_iterations.emplace(i, owner.decode_iteration);
    }

    for (const auto & markers : prefills_by_pid | std::views::values) {
        audit.unmatched_prefill_marker_count += std::ranges::count_if(markers, [&](const auto & marker) { return !used_prefills.contains(marker.event); });
    }
    for (const auto & markers : decodes_by_pid | std::views::values) {
        audit.unmatched_decode_marker_count += std::ranges::count_if(markers, [&](const auto & marker) { return !used_decodes.contains(marker.event); });
    }
    std::vector<size_t> order(audit.observations.size()), positions(order.size());
    std::iota(order.begin(), order.end(), 0);
    std::ranges::sort(order, [&](size_t a, size_t b) {
        const auto & left = audit.observations[a];
        const auto & right = audit.observations[b];
        if (left.prefill_start_us != right.prefill_start_us) return left.prefill_start_us < right.prefill_start_us;
        if (left.logical_input != right.logical_input) return left.logical_input < right.logical_input;
        return left.pid < right.pid;
    });
    std::vector<HiCachePhaseObservation> sorted;
    sorted.reserve(order.size());
    for (size_t i = 0; i < order.size(); ++i) {
        positions[order[i]] = i;
        sorted.push_back(std::move(audit.observations[order[i]]));
    }
    audit.observations = std::move(sorted);
    for (auto & interval : phase_intervals) interval.observation_index = positions[interval.observation_index];
    const bool complete = audit.cache_extend_fact_count > 0 && audit.cache_extend_fact_count == audit.request_bound_fact_count
                          && audit.cache_extend_fact_count == audit.paired_prefill_count && audit.invalid_fact_count == 0 && audit.token_range_error_count == 0
                          && audit.unmatched_prefill_marker_count == 0 && audit.unmatched_decode_marker_count == 0 && audit.phase_owner_conflict_count == 0;
    audit.status = complete ? "ready" : "not_ready";
    return result;
}

} // namespace markov::trace_graph::modules::hicache
