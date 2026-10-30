#include "markov/adapters/sglang/observations.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace markov::adapters::sglang {

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
