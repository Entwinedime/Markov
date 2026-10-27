#include "markov/adapters/source.hpp"

#include "markov/adapters/ascend/trace.hpp"
#include "markov/adapters/sglang/observations.hpp"
#include "markov/trace_graph/source/chrome_reader.hpp"
#include "markov/trace_graph/source/clients.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace markov::adapters {

PreparedSource prepare_source(trace_graph::source::ProfileManifest manifest) {
    using namespace trace_graph;
    using Clock = std::chrono::steady_clock;
    const auto begin = Clock::now();
    source::Observations observations;
    ascend::RuntimeCausality causality;
    sglang::ProbeReader probes(observations);
    std::vector<std::size_t> counts;
    for (std::size_t file_id = 0; file_id < manifest.files.size(); ++file_id) {
        const auto & file = manifest.files[file_id];
        std::size_t records = 0;
        if (file.channel == source::Channel::Client) {
            const auto requests = source::read_client_requests(file.path);
            if (!observations.requests.empty() && !requests.empty() && requests.front().begin_ns < observations.requests.back().end_ns)
                throw std::runtime_error("Client reports do not form a serial request sequence: " + file.path.string());
            observations.requests.insert(observations.requests.end(), requests.begin(), requests.end());
            records = requests.size();
        }
        else if (file.channel == source::Channel::PythonProbe) {
            if (manifest.framework != "sglang") throw std::runtime_error("Framework probe interpretation is not implemented for " + manifest.framework);
            probes.set_file(file_id);
            records = source::read_chrome_trace(file.path, probes);
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
        const auto ports = probes.requests.find(request.identity);
        if (ports != probes.requests.end() && ports->second.received.size() == 1 && ports->second.sent.size() == 1) {
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
             std::move(probes.roles),
             std::chrono::duration<double>(parsed - begin).count(),
             std::chrono::duration<double>(built - parsed).count() };
}

} // namespace markov::adapters
