#include "markov/trace_graph/source/clients.hpp"

#include "markov/trace_graph/source/chrome_reader.hpp"

#include <fstream>
#include <stdexcept>

namespace markov::trace_graph::source {

std::vector<RequestWindow> read_client_requests(const std::filesystem::path & path) {
    std::ifstream stream(path);
    const auto report = nlohmann::json::parse(stream);
    std::vector<RequestWindow> result;
    Time prior_end = 0;
    for (const auto & row : report.at("requests")) {
        if (row.at("kind") != "request") continue;
        if (row.at("status") != "ok") throw std::runtime_error("Base client report contains a failed request: " + path.string());

        // Client reports store milliseconds, whereas Chrome stores us.
        const auto begin = nanoseconds_from_ms(row.at("start_time_ms"));
        const auto end = nanoseconds_from_ms(row.at("end_time_ms"));
        if (begin < prior_end || end <= begin) throw std::runtime_error("Source HTTP requests are not a valid serial sequence");

        result.push_back({ row.at("logical_request_id").get<std::string>(), begin, end, row.at("measure").get<bool>() });
        prior_end = end;
    }
    return result;
}

} // namespace markov::trace_graph::source
