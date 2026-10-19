#include "markov/adapters/runtimes/ascend/source_extractor.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace markov {

namespace {

// Typed observations are shared by every skipped-wait query in this pass.
// Event signals added during the pass do not change native task dependencies.
class EventWaitFrontiers {
public:
    using DeviceOrder = std::map<SourceSpanId, std::pair<std::size_t, std::size_t>>;

    EventWaitFrontiers(const nlohmann::json & intervals, const SourceGraph & graph, const DeviceOrder & order) : graph_(graph), order_(order) {
        std::map<std::pair<std::uint32_t, std::uint64_t>, std::vector<SourceSpanId>> submissions;
        for (const auto & interval : intervals) {
            if (interval.at("process") == "CANN")
                submissions[{ interval.at("rank"), interval.at("arguments").at("connection_id") }].push_back(interval.at("id"));
        }
        for (const auto & interval : intervals) {
            if (interval.at("process") != "Ascend Hardware") continue;
            const auto rank = interval.at("rank").get<std::uint32_t>();
            const auto stream = interval.at("arguments").at("Physic Stream Id").get<std::uint64_t>();
            const auto & matches = submissions[{ rank, interval.at("arguments").at("connection_id") }];
            Task task{ interval.at("id"), std::nullopt };
            if (matches.size() == 1) {
                const auto & submission = intervals.at(matches.front());
                task.submission = Submission{ submission.at("tid"), submission.at("start_ns"), submission.at("end_ns") };
            }
            streams_[{ rank, stream }].push_back(task);
        }

        for (const auto & edge : graph.edges())
            if (edge.relation == SourceRelation::NativeTaskDependency) successors_[edge.source].push_back(edge.target);
    }

    std::expected<std::vector<SourceSpanId>, std::string> resolve(const nlohmann::json & record, const nlohmann::json & intervals) const {
        if (record.at("source_interval").is_null() || record.at("stream_id").is_null()) return std::unexpected("event_wait_submission_not_observed");
        const auto & wait = intervals.at(record.at("source_interval").get<std::size_t>());
        const auto tid = wait.at("tid").get<std::uint64_t>();
        const auto start_ns = wait.at("start_ns").get<std::int64_t>();
        const auto end_ns = wait.at("end_ns").get<std::int64_t>();
        const auto stream = streams_.find({ record.at("rank"), record.at("stream_id") });
        if (stream == streams_.end()) return std::unexpected("event_wait_consumer_frontier_not_observed");

        // Same-thread CPU submission order identifies the cut. Device times
        // and returned task identifiers cannot stand in for a skipped wait.
        struct Frontier {
            std::size_t position;
            std::vector<SourceSpanId> spans;
        };
        std::map<std::size_t, Frontier> after;
        std::map<std::size_t, std::size_t> before;
        for (const auto & task : stream->second) {
            if (!task.submission) return std::unexpected("event_wait_stream_submission_not_unique");
            const auto & submission = *task.submission;
            if (submission.tid != tid) return std::unexpected("event_wait_concurrent_submission_not_qualified");

            const auto [chain, position] = order_.at(task.span);
            if (submission.end_ns <= start_ns) {
                auto previous = before.try_emplace(chain, position).first;
                previous->second = std::max(previous->second, position);
            }
            else if (submission.start_ns >= end_ns) {
                auto & frontier = after.try_emplace(chain, Frontier{ position, {} }).first->second;
                if (position < frontier.position) frontier = { position, {} };
                if (position == frontier.position) frontier.spans.push_back(task.span);
            }
            else return std::unexpected("event_wait_overlapping_submission_not_qualified");
        }
        if (after.empty()) return std::unexpected("event_wait_consumer_frontier_not_observed");

        std::map<BoundaryId, SourceSpanId> entries;
        std::set<BoundaryId> ends;
        for (const auto & [chain, frontier] : after) {
            const auto previous = before.find(chain);
            if (previous != before.end() && previous->second >= frontier.position) return std::unexpected("event_wait_submission_order_disagrees");
            for (const auto span : frontier.spans) {
                entries.emplace(graph_.spans()[span].begin, span);
                ends.insert(graph_.spans()[span].end);
            }
        }

        // Compound launches can fork; only their native dependency roots need
        // the event gate. Other sub-tasks inherit it through descriptor edges.
        for (const auto end : ends) {
            const auto next = successors_.find(end);
            if (next != successors_.end())
                for (const auto target : next->second) entries.erase(target);
        }
        std::vector<SourceSpanId> frontier;
        for (const auto & [boundary, span] : entries) frontier.push_back(span);
        return frontier;
    }

private:
    struct Submission {
        std::uint64_t tid;
        std::int64_t start_ns;
        std::int64_t end_ns;
    };
    struct Task {
        SourceSpanId span;
        std::optional<Submission> submission;
    };

    const SourceGraph & graph_;
    const DeviceOrder & order_;
    std::map<std::pair<std::uint32_t, std::uint64_t>, std::vector<Task>> streams_;
    std::map<BoundaryId, std::vector<BoundaryId>> successors_;
};

} // namespace

void AscendSourceExtractor::read_event_signals(const nlohmann::json & records, const nlohmann::json & intervals) {
    const EventWaitFrontiers frontiers(intervals, graph_, device_order_);
    struct Signal {
        std::size_t api;
        std::int64_t returned_ns;
        std::vector<SourceSpanId> spans;
    };
    using EventKey = std::pair<std::uint64_t, std::uint64_t>; // Event handles are process-local and may be reused after destroy.
    std::map<EventKey, Signal> signals;
    std::set<SourceSpanId> unresolved;
    for (const auto & interval : intervals) {
        if (interval.at("process") == "Ascend Hardware" && interval.at("name") == "EVENT_WAIT") { unresolved.insert(interval.at("id").get<SourceSpanId>()); }
    }

    std::vector<std::size_t> sequence(records.size());
    std::iota(sequence.begin(), sequence.end(), 0);
    std::sort(sequence.begin(), sequence.end(), [&records](std::size_t left, std::size_t right) {
        return records[left].at("start_ns").get<std::int64_t>() < records[right].at("start_ns").get<std::int64_t>();
    });

    std::map<EventKey, std::vector<std::int64_t>> changes;
    for (const auto index : sequence) {
        const auto & record = records[index];
        const auto name = record.at("api").get<std::string>();
        if (record.at("result").get<std::int32_t>() == 0 && (name == "rtEventRecord" || name == "rtEventDestroy")) {
            changes[EventKey(record.at("pid").get<std::uint64_t>(), record.at("event_handle").get<std::uint64_t>())].push_back(
                record.at("start_ns").get<std::int64_t>());
        }
    }
    for (const auto index : sequence) {
        const auto & record = records[index];
        if (record.at("result").get<std::int32_t>() != 0) continue;

        const auto name = record.at("api").get<std::string>();
        const EventKey key(record.at("pid").get<std::uint64_t>(), record.at("event_handle").get<std::uint64_t>());
        if (name == "rtEventDestroy") {
            signals.erase(key);
            continue;
        }
        if (name == "rtEventRecord") {
            signals.insert_or_assign(
                key,
                Signal{ index, record.at("end_ns").get<std::int64_t>(), record.at("hardware_intervals").get<std::vector<SourceSpanId>>() });
            continue;
        }
        if (name != "rtStreamWaitEvent") continue;

        const auto producer = signals.find(key);
        // A completed record call before wait entry identifies its captured
        // generation. Concurrent record/wait calls need another causal fact;
        // timestamps alone cannot pick a generation or recover a missing task.
        if (producer == signals.end() || producer->second.returned_ns > record.at("start_ns").get<std::int64_t>() || producer->second.spans.empty()) continue;

        const auto & mutations = changes[key];
        const auto concurrent = std::lower_bound(mutations.begin(), mutations.end(), record.at("start_ns").get<std::int64_t>());
        if (concurrent != mutations.end() && *concurrent < record.at("end_ns").get<std::int64_t>()) continue;

        if (record.at("hardware_intervals").empty()) {
            const auto frontier = frontiers.resolve(record, intervals);
            if (!frontier) {
                gaps_.push_back({
                    {     "kind",    "observation" },
                    {   "reason", frontier.error() },
                    { "wait_api",            index }
                });
                continue;
            }
            // A successful wait gates later stream work even when no separate
            // hardware wait was recorded. Preserve the API contract as edges,
            // without inventing a service interval or a measured duration.
            for (const auto target : *frontier) {
                for (const auto source : producer->second.spans)
                    graph_.connect(graph_.spans()[source].end, graph_.spans()[target].begin, SourceRelation::NativeEventSignal, producer->second.api);
            }
            continue;
        }

        for (const auto wait : record.at("hardware_intervals").get<std::vector<SourceSpanId>>()) {
            for (const auto source : producer->second.spans) {
                graph_.connect(graph_.spans()[source].end, graph_.spans()[wait].end, SourceRelation::NativeEventSignal, producer->second.api);
            }
            unresolved.erase(wait);
        }
    }

    if (!unresolved.empty()) {
        gaps_.push_back({
            {      "kind",                                 "observation" },
            {    "reason", "native_event_wait_producer_identity_missing" },
            { "intervals",                             unresolved.size() }
        });
    }
}

} // namespace markov
