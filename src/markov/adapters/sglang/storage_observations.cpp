#include "markov/adapters/sglang/observations.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace markov::adapters::sglang {

void ProbeReader::observe_prefetch_query(const ProbeEvent & event) {
    using namespace trace_graph::source;
    const auto & fields = event.args;
    if (fields.at("status") != "returned") throw std::runtime_error("Cannot interpret a failed background prefetch query");
    const auto requests = fields.at("request_ids").get<std::vector<std::string>>();
    if (requests.size() != 1) throw std::runtime_error("Background prefetch query must belong to one request");
    const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + event.process + ":" + event.thread);
    prefetch_queries_[output_.name(requests.front())].push_back({
        { resource, event.begin_ns, event.end_ns, event.record },
        fields.at("participant").get<Index>(),
        fields.at("participants").get<Index>(),
        fields.at("queued_at_ns").get<Time>(),
        fields.at("features").get<trace_graph::planning::Features>()
    });
}

void ProbeReader::finish_prefetch_queries() {
    using namespace trace_graph;
    std::vector<decltype(prefetch_queries_)::value_type *> ordered;
    for (auto & item : prefetch_queries_) ordered.push_back(&item);
    std::ranges::sort(ordered, {}, [](const auto * item) {
        return std::ranges::max_element(item->second, {}, [](const PrefetchQuery & query) { return query.region.begin_ns; })->region.begin_ns;
    });
    source::Time previous_end = 0;
    for (const auto * item : ordered) {
        const auto & [request, queries] = *item;
        const auto participants = queries.front().participants;
        std::set<source::Index> ranks;
        planning::Features features;
        auto begin = previous_end;
        source::Time end = 0;
        for (const auto & query : queries) {
            if (query.participants != participants || query.participant >= participants || !ranks.insert(query.participant).second)
                throw std::runtime_error("Background query has inconsistent or repeated participants");
            begin = std::max(begin, query.queued_at_ns);
            end = std::max(end, query.region.end_ns);
            for (const auto & [key, value] : query.features) features[key] = std::max(features[key], value);
        }
        if (queries.size() != participants) {
            ++output_.counts["prefetch_query_incomplete_group"];
            previous_end = std::max(previous_end, end);
            continue;
        }
        if (end < begin) throw std::runtime_error("Background query response precedes its eligible enqueue");
        const auto representative = std::ranges::find(queries, source::Index{ 0 }, &PrefetchQuery::participant);
        auto region = representative->region;
        region.begin_ns = begin;
        region.end_ns = end;
        output_.cost_observations.push_back({
            request,
            0,
            0,
            "storage_query",
            { { "cache.prefetch.query", "sglang.file.prefetch_query", "query_group_response" }, std::move(features) },
            end - begin,
            region,
            std::nullopt,
            false
        });
        previous_end = end;
        ++output_.counts["prefetch_query_group_response"];
    }
    prefetch_queries_.clear();
}

void ProbeReader::observe_storage(const ProbeEvent & event) {
    using namespace trace_graph;
    const auto & fields = event.args;
    ++output_.counts["storage_observation." + fields.at("status").get<std::string>()];
    if (fields.at("status") != "returned") return;

    const auto request = fields.at("request_id").is_null() ? std::nullopt : std::optional(output_.name(fields.at("request_id").get<std::string>()));
    const auto iteration = fields.at("iteration").get<std::size_t>();
    const auto rank = fields.at("participant").get<std::size_t>();
    const auto tokens = fields.at("prefix_tokens").get<std::vector<std::int64_t>>();
    const auto page_bytes = fields.at("page_bytes").get<double>();
    const auto implementation = fields.at("implementation").get<std::string>();
    const auto resource = output_.resource(source::ResourceKind::CpuThread, "cpu:" + event.process + ":" + event.thread);
    source::Time accounted = 0;
    for (const auto & piece : fields.at("costs")) {
        const auto end_token = piece.at("end_token").get<std::size_t>();
        const auto duration = piece.at("duration_ns").get<source::Time>();
        const auto begin = piece.at("begin_offset_ns").get<source::Time>();
        const auto end = piece.at("end_offset_ns").get<source::Time>();
        if (end_token > tokens.size() || begin < 0 || end < begin || end > event.end_ns - event.begin_ns || duration < 0 || duration > end - begin
            || duration > event.end_ns - event.begin_ns - accounted)
            throw std::runtime_error("File service measurement exceeds its recorded token or time envelope");
        accounted += duration;

        const auto pages = piece.at("pages").get<double>();
        planning::Work work{
            { piece.at("operation").get<std::string>(), implementation, "storage_worker_service" },
            { { "bytes", pages * page_bytes }, { "pages", pages }, { "operations", 1 }, { "page_bytes", page_bytes } }
        };
        output_.cost_observations.push_back({
            request,
            iteration,
            rank,
            event.name == "storage.file.read" ? "storage_read" : "storage_write",
            std::move(work),
            duration,
            { resource, event.begin_ns + begin, event.begin_ns + end, event.record },
            std::vector<std::int64_t>(tokens.begin(), tokens.begin() + end_token),
            piece.at("contiguous").get<bool>()
        });
    }
    if (accounted != event.end_ns - event.begin_ns) throw std::runtime_error("File service pieces do not cover the native batch exactly once");
}

} // namespace markov::adapters::sglang
