#include "markov/trace_graph/source/clients.hpp"

#include "markov/trace_graph/source/chrome_reader.hpp"

#include <fstream>
#include <stdexcept>

namespace markov::trace_graph::source {

ClientProgram read_client_program(const std::filesystem::path & path, Index file) {
    std::ifstream stream(path);
    const auto report = nlohmann::json::parse(stream);
    ClientProgram result{ {}, {}, 0 };
    Time prior_end = 0;
    for (const auto & row : report.at("requests")) {
        const auto position = result.record_count++;
        const auto & kind = row.at("kind");
        if (kind == "startup_gate" || kind == "checkpoint") continue;
        if (kind == "barrier") {
            if (row.at("status") != "ok" || row.at("scope") != "hicache_idle")
                throw std::runtime_error("Unsupported or failed workload barrier: " + path.string());

            result.barriers.push_back({
                result.requests.size(),
                BarrierKind::DrainHiCache,
                { file, position }
            });
            continue;
        }
        if (kind != "request") throw std::runtime_error("Unknown client program step: " + kind.get<std::string>());
        if (row.at("status") != "ok") throw std::runtime_error("Base client report contains a failed request: " + path.string());

        // Client reports store milliseconds, whereas Chrome stores us.
        const auto begin = nanoseconds_from_ms(row.at("start_time_ms"));
        const auto end = nanoseconds_from_ms(row.at("end_time_ms"));
        if (begin < prior_end || end <= begin) throw std::runtime_error("Source HTTP requests are not a valid serial sequence");

        result.requests.push_back({ row.at("logical_request_id").get<std::string>(), begin, end, row.at("measure").get<bool>() });
        prior_end = end;
    }
    return result;
}

} // namespace markov::trace_graph::source
