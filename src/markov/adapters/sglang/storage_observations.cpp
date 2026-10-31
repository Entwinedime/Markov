#include "markov/adapters/sglang/observations.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace markov::adapters::sglang {

void ProbeReader::observe_prefetch_response(const ProbeEvent & event) {
    using namespace trace_graph::source;
    const auto & fields = event.args;
    if (fields.at("status") != "returned") throw std::runtime_error("Cannot interpret a failed prefetch response");
    const auto requests = fields.at("request_ids").get<std::vector<std::string>>();
    if (requests.size() != 1) throw std::runtime_error("Prefetch response must belong to one request");
    const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + event.process + ":" + event.thread);
    prefetch_responses_[{
                            event.name,
                            output_.name(requests.front())
    }]
        .push_back({ { resource, event.begin_ns, event.end_ns, event.record },
                     fields.at("participant").get<Index>(),
                     fields.at("participants").get<Index>(),
                     event.name == "prefetch.query" ? std::optional(fields.at("queued_at_ns").get<Time>()) : std::nullopt,
                     fields.at("features").get<trace_graph::planning::Features>() });
}

void ProbeReader::finish_prefetch_responses() {
    using namespace trace_graph;
    std::vector<decltype(prefetch_responses_)::value_type *> ordered;
    for (auto & item : prefetch_responses_) ordered.push_back(&item);
    std::ranges::sort(ordered, {}, [](const auto * item) {
        return std::ranges::max_element(item->second, {}, [](const PrefetchResponse & response) { return response.region.begin_ns; })->region.begin_ns;
    });
    source::Time previous_query_end = 0;
    for (const auto * item : ordered) {
        const auto & [identity, responses] = *item;
        const auto & [name, request] = identity;
        const auto query = name == "prefetch.query";
        const auto participants = responses.front().participants;
        std::set<source::Index> ranks;
        planning::Features features;
        auto begin = query ? previous_query_end : 0;
        source::Time end = 0;
        for (const auto & response : responses) {
            if (response.participants != participants || response.participant >= participants || !ranks.insert(response.participant).second)
                throw std::runtime_error("Prefetch response has inconsistent or repeated participants");
            begin = std::max(begin, query ? *response.queued_at_ns : response.region.begin_ns);
            end = std::max(end, response.region.end_ns);
            for (const auto & [key, value] : response.features) features[key] = std::max(features[key], value);
        }
        if (query) previous_query_end = std::max(previous_query_end, end);
        if (responses.size() != participants) {
            ++output_.counts[name + ".incomplete_group"];
            continue;
        }
        if (end < begin) throw std::runtime_error("Prefetch response precedes its group start");
        const auto representative = std::ranges::find(responses, source::Index{ 0 }, &PrefetchResponse::participant);
        auto region = representative->region;
        region.begin_ns = begin;
        region.end_ns = end;
        std::string_view port = "storage_query";
        if (name == "prefetch.issue") port = "storage_issue";
        else if (name == "prefetch.check") port = "storage_check";
        else if (name == "prefetch.accept") port = "storage_accept";
        const planning::WorkKind kind{ "cache." + name,
                                       query ? "sglang.file.prefetch_query" : "sglang.HiRadixCache.prefetch",
                                       query ? "query_group_response" : "controller_group_response" };
        output_.cost_observations.push_back({
            request,
            0,
            0,
            port,
            { kind, std::move(features) },
            end - begin,
            region,
            std::nullopt,
            false
        });
        ++output_.counts[name + ".group_response"];
    }
    prefetch_responses_.clear();
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
