#include "markov/adapters/source.hpp"

#include "markov/adapters/ascend/trace.hpp"
#include "markov/adapters/probes.hpp"
#include "markov/adapters/sglang/observations.hpp"
#include "markov/trace_graph/source/chrome_reader.hpp"
#include "markov/trace_graph/source/clients.hpp"

#include <chrono>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <utility>

namespace markov::adapters {

PageWorkRequest read_page_work_request(const std::filesystem::path & path) {
    std::ifstream stream(path);
    const auto raw = nlohmann::json::parse(stream);
    for (const auto & [name, value] : raw.items()) {
        if (name != "manifest" && name != "page_sizes_tokens") throw std::runtime_error("Unknown page-work input field: " + name);
    }
    auto manifest_path = std::filesystem::path(raw.at("manifest").get<std::string>());
    if (manifest_path.is_relative()) manifest_path = path.parent_path() / manifest_path;

    PageWorkRequest result{ trace_graph::source::inspect_manifest(manifest_path), {} };
    const auto & sizes = raw.at("page_sizes_tokens");
    if (!sizes.is_array() || sizes.empty()) throw std::runtime_error("Page-work requires a nonempty page-size array");
    for (const auto & value : sizes) {
        if (!value.is_number_integer()) throw std::runtime_error("Target page size must be an integer");
        const auto size = value.get<std::int64_t>();
        if (size <= 0) throw std::runtime_error("Target page size must be positive");
        result.page_sizes_tokens.push_back(static_cast<std::size_t>(size));
    }
    return result;
}

PreparedSource prepare_source(trace_graph::source::ProfileManifest manifest) {
    using namespace trace_graph;
    using Clock = std::chrono::steady_clock;
    const auto begin = Clock::now();
    source::Observations observations;
    ascend::RuntimeCausality causality;
    std::unique_ptr<ProbeReader> probes;
    if (manifest.framework == "sglang") probes = std::make_unique<sglang::ProbeReader>(observations);
    else if (manifest.framework == "ktransformers") probes = std::make_unique<ProbeReader>(observations);
    else throw std::runtime_error("Framework probe interpretation is not implemented for " + manifest.framework);
    std::vector<std::size_t> counts;
    for (std::size_t file_id = 0; file_id < manifest.files.size(); ++file_id) {
        const auto & file = manifest.files[file_id];
        std::size_t records = 0;
        if (file.channel == source::Channel::Client) {
            auto program = source::read_client_program(file.path, file_id);
            if (!observations.requests.empty() && !program.requests.empty() && program.requests.front().begin_ns < observations.requests.back().end_ns)
                throw std::runtime_error("Client reports do not form a serial request sequence: " + file.path.string());

            for (auto & barrier : program.barriers) {
                barrier.before_request += observations.requests.size();
                observations.workload_barriers.push_back(barrier);
            }
            observations.requests.insert(observations.requests.end(),
                                         std::make_move_iterator(program.requests.begin()),
                                         std::make_move_iterator(program.requests.end()));
            records = program.record_count;
        }
        else if (file.channel == source::Channel::PythonProbe) {
            probes->set_file(file_id);
            records = source::read_chrome_trace(file.path, *probes);
        }
        else {
            ascend::TraceReader reader(observations, causality, file, file_id);
            records = source::read_chrome_trace(file.path, reader);
            reader.finish();
        }
        counts.push_back(records);
    }
    observations.normalize_resources();
    observations.resolve_flows();
    causality.connect(observations);
    for (auto & request : observations.requests) {
        const auto ports = probes->requests.find(request.identity);
        if (ports != probes->requests.end() && ports->second.received.size() == 1 && ports->second.sent.size() == 1) {
            request.received = ports->second.received.front();
            request.sent = ports->second.sent.front();
        }
    }

    const auto parsed = Clock::now();
    if (observations.spans.empty()) throw std::runtime_error("No execution spans were observed in the declared source inputs");
    source::SourceContext context(std::move(observations));
    const auto built = Clock::now();
    return { std::move(manifest),
             std::move(context),
             std::move(counts),
             std::move(probes->roles),
             std::chrono::duration<double>(parsed - begin).count(),
             std::chrono::duration<double>(built - parsed).count() };
}

} // namespace markov::adapters
