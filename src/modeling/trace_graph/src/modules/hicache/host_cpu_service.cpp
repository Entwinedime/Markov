#include "markov/trace_graph/modules/hicache/host_cpu_service.hpp"
#include "markov/trace_graph/modules/hicache/layer_waits.hpp"
#include "markov/trace_graph/modules/hicache/patch/source_dag_index.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>

namespace markov::trace_graph::modules::hicache {
namespace {
struct Piece {
    uint64_t begin, end;
    bool sync;
};

// Match Python round rather than llround (which rounds ties away from zero).
int64_t rounded_change(double value) {
    if (!std::isfinite(value) || value <= static_cast<double>(std::numeric_limits<int64_t>::min())
        || value >= static_cast<double>(std::numeric_limits<int64_t>::max()))
        throw std::runtime_error("Host CPU change is outside the supported finite range");
    auto integral = static_cast<int64_t>(std::floor(value));
    const auto fraction = value - std::floor(value);
    if (fraction > .5 || (fraction == .5 && integral % 2 != 0)) ++integral;
    return integral;
}
} // namespace

nlohmann::json attach_host_cpu_service(core::DagGraph & graph, const nlohmann::json & measurements) {
    auto retained = nlohmann::json::array();
    const patch::HiCacheSourceDagIndex source(graph);
    const bool has_layers =
        std::ranges::any_of(measurements.at("rows"), [](const auto & row) { return row.value("method", std::string{}) == "hicache.layer_wait"; });
    const auto layers = has_layers ? observe_hicache_layer_waits(source) : HiCacheLayerWaitObservation{};
    if (has_layers && layers.status != "ready") throw std::runtime_error("Layer CPU service requires aligned source call observations");
    using Lane = std::pair<std::string, std::string>;
    const auto identity = [](const nlohmann::json & value) { return value.is_string() ? value.get<std::string>() : std::to_string(value.get<uint64_t>()); };
    for (const auto & call : measurements.at("rows")) {
        const Lane lane{ identity(call.at("pid")), identity(call.at("tid")) };
        auto ranges = call.at("exclusive_ranges_ns");
        if (call.value("method", std::string{}) == "hicache.layer_wait") {
            // CPU deltas use paired thread timers. Source geometry must use
            // the profiler clock, joined by identity rather than wall proximity.
            std::map<uint64_t, std::vector<const HiCacheLayerWaitCall *>> batches;
            for (const auto & observed : layers.calls) {
                if (observed.request_id != call.at("request_id").get<std::string>() || "step[" + observed.phase != call.at("phase").get<std::string>()
                    || !observed.before)
                    continue;
                const auto & event = graph.event_for_node(*observed.before);
                if (event.pid == lane.first && event.tid == lane.second) batches[observed.batch_start_ns].push_back(&observed);
            }
            const auto ordinal = call.value("forward_ordinal", size_t{ 0 });
            if (ordinal >= batches.size()) throw std::runtime_error("Layer CPU forward ordinal is absent from source");
            auto & selected = std::next(batches.begin(), ordinal)->second;
            std::ranges::sort(selected, {}, [](const auto * observed) { return observed->start_ns; });
            const auto & expected = call.at("layer_calls");
            if (selected.empty() || selected.size() != expected.size()) throw std::runtime_error("Layer CPU source calls differ from measured work");
            ranges = nlohmann::json::array();
            for (size_t i = 0; i < selected.size(); ++i) {
                const auto & observed = *selected[i];
                if (!observed.issue.empty() || observed.batch_start_ns != selected.front()->batch_start_ns || observed.layer != expected[i][0].get<uint64_t>()
                    || observed.enabled != expected[i][1].get<bool>())
                    throw std::runtime_error("Layer CPU source identity or active state differs");
                if (observed.end_ns / 1'000 > observed.start_ns / 1'000) ranges.push_back({ observed.start_ns, observed.end_ns });
            }
            if (ranges.empty() && call.at("measured_service_delta_us").get<double>() == 0) continue;
        }
        std::vector<Piece> pieces;
        bool retain_recorder = false;
        for (const auto & range : ranges) {
            const auto begin = range.at(0).get<uint64_t>() / 1'000, end = range.at(1).get<uint64_t>() / 1'000;
            if (end <= begin) throw std::runtime_error("Host CPU range is empty at graph resolution");
            const auto owned = source.timing_interval_ownership(lane.first, lane.second, begin, end - begin);
            std::vector<Piece> spans;
            for (const auto & gap : owned.owned_gap_slices) spans.push_back({ gap.owned_start_us, gap.owned_end_us, false });
            for (const auto id : owned.owned_node_ids) {
                const auto & event = graph.event_for_node(id);
                spans.push_back({ event.ts, event.ts + event.dur, event.name.starts_with("AscendCL@aclrtSynchronize") });
            }
            std::vector<size_t> crossing;
            for (const auto id : source.cpu_interval_candidates(lane.first, lane.second, begin, end)) {
                const auto & event = graph.event_for_node(id);
                const auto stop = event.ts + event.dur;
                if (event.ts >= end || stop <= begin || (event.ts >= begin && stop <= end)) continue;
                crossing.push_back(id);
            }
            // Keep the previous source-node order for rounding and diagnostics;
            // only the boundary candidates, not the entire lane, need sorting.
            std::ranges::sort(crossing);
            for (const auto id : crossing) {
                const auto & event = graph.event_for_node(id);
                const auto stop = event.ts + event.dur;
                if (event.cat != "cpu_op" || !event.name.ends_with(".self")) {
                    if (call.value("method", std::string{}) == "profiling.recorder_write") {
                        // Do not distribute this CPU budget across a semantic/wait
                        // boundary. Retain the whole write, including any earlier pieces.
                        retained.push_back({
                            {           "measurement",                                                               call },
                            {                "reason",                                         "nonordinary_cpu_boundary" },
                            {                  "node",                                                         event.name },
                            {         "node_begin_us",                                                           event.ts },
                            {           "node_end_us",                                                               stop },
                            {        "range_begin_us",                                                              begin },
                            {          "range_end_us",                                                                end },
                            { "retained_reduction_us", rounded_change(call.at("measured_service_delta_us").get<double>()) }
                        });
                        retain_recorder = true;
                        break;
                    }
                    throw std::runtime_error("Host CPU boundary is not an ordinary self slice: method=" + call.value("method", std::string{})
                                             + " pid=" + lane.first + " tid=" + lane.second + " range=" + std::to_string(begin) + ":" + std::to_string(end)
                                             + " node=" + event.name + " interval=" + std::to_string(event.ts) + ":" + std::to_string(stop));
                }
                spans.push_back({ std::max(begin, event.ts), std::min(end, stop), false });
            }
            if (retain_recorder) break;
            auto ordered = spans;
            std::ranges::sort(ordered, [](const auto & a, const auto & b) { return std::pair{ a.begin, a.end } < std::pair{ b.begin, b.end }; });
            auto cursor = begin;
            for (const auto & piece : ordered) {
                if (piece.begin != cursor || piece.end <= cursor) throw std::runtime_error("Host CPU ownership has gaps or overlapping slices");
                cursor = piece.end;
            }
            if (cursor != end) throw std::runtime_error("Host CPU ownership is incomplete");
            for (const auto & piece : spans)
                if (!piece.sync) pieces.push_back(piece);
        }
        if (retain_recorder) continue;
        uint64_t total = 0;
        for (const auto & piece : pieces) total += piece.end - piece.begin;
        const auto reduction = rounded_change(call.at("measured_service_delta_us").get<double>());
        if (!total || static_cast<__int128>(reduction) > total) throw std::runtime_error("Host CPU reduction exceeds eligible source service");
        uint64_t cumulative = 0;
        __int128 assigned = 0;
        for (const auto & piece : pieces) {
            const auto width = piece.end - piece.begin;
            cumulative += width;
            const auto numerator = static_cast<__int128>(cumulative) * reduction;
            auto projected = numerator / total;
            if (numerator < 0 && numerator % total) --projected;
            const auto service = static_cast<__int128>(width) - (projected - assigned);
            if (service < 0 || service > std::numeric_limits<uint64_t>::max()) throw std::runtime_error("Host CPU service exceeds integer range");
            graph.cpu_service_cost().add(lane, { piece.begin, piece.end, static_cast<uint64_t>(service) });
            assigned = projected;
        }
        if (assigned != reduction) throw std::runtime_error("Host CPU allocation changed the measured budget");
    }
    return retained;
}

nlohmann::json attach_recorder_cpu_service(core::DagGraph & graph, const nlohmann::json & writes) {
    auto rows = nlohmann::json::array();
    core::CpuServiceCost selected_ranges;
    double cpu_us = 0;
    int64_t reduction_us = 0;
    for (const auto & write : writes) {
        const auto identity = [](const nlohmann::json & value) { return value.is_string() ? value.get<std::string>() : std::to_string(value.get<uint64_t>()); };
        const core::CpuServiceCost::Lane lane{ identity(write.at("pid")), identity(write.at("tid")) };
        const auto begin_ns = write.at("method_begin_ns").get<uint64_t>();
        const auto end_ns = write.at("method_end_ns").get<uint64_t>();
        const auto begin = begin_ns / 1'000, end = end_ns / 1'000;
        if (end <= begin) throw std::runtime_error("Recorder write is smaller than graph resolution");

        // Inspect the already resolved forward/host spans, before adding any
        // recorder intervals. Overlap among new writes remains an input error.
        const auto found = graph.cpu_service_cost().lanes().find(lane);
        if (found != graph.cpu_service_cost().lanes().end()) {
            const auto & spans = found->second;
            const auto at = std::lower_bound(spans.begin(), spans.end(), begin, [](const auto & span, uint64_t time) { return span.end_us <= time; });
            if (at != spans.end() && at->begin_us < end) continue;
        }
        const auto cpu = write.at("thread_cpu_ns").get<double>() / 1'000;
        const auto reduction = rounded_change(cpu);
        if (reduction < 0 || static_cast<uint64_t>(reduction) > end - begin)
            throw std::runtime_error("Measured recorder CPU does not fit graph time resolution");
        selected_ranges.add(lane, { begin, end, 0 });
        rows.push_back({
            {                       "pid",            write.at("pid") },
            {                       "tid",            write.at("tid") },
            {                    "method", "profiling.recorder_write" },
            {             "writer_method",         write.at("method") },
            {       "exclusive_ranges_ns",   { { begin_ns, end_ns } } },
            {   "profiled_service_cpu_us",                        cpu },
            {     "normal_service_cpu_us",                          0 },
            { "measured_service_delta_us",                        cpu }
        });
        cpu_us += cpu;
        reduction_us += reduction;
    }
    const auto retained = rows.empty() ? nlohmann::json::array()
                                       : attach_host_cpu_service(graph,
                                                                 {
                                                                     { "rows", rows }
    });
    int64_t retained_us = 0;
    for (const auto & row : retained) retained_us += row.at("retained_reduction_us").get<int64_t>();
    return {
        {                             "writes",                                                         rows.size() },
        {                       "added_cpu_us",                                                              cpu_us },
        {         "added_integer_reduction_us",                                                        reduction_us },
        {                    "retained_writes",                                                     retained.size() },
        {                    "retained_cpu_us",                                                         retained_us },
        { "applied_added_integer_reduction_us",                                          reduction_us - retained_us },
        {                              "scope", "Measured uncovered source recorder writes; unknown costs retained" },
        {                               "rows",                                                            retained }
    };
}
} // namespace markov::trace_graph::modules::hicache
