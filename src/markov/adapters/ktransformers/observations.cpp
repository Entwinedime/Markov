#include "markov/adapters/ktransformers/observations.hpp"

#include <stdexcept>

namespace markov::adapters::ktransformers {

void ProbeReader::observe(const ProbeEvent & event) {
    using namespace trace_graph::source;
    if (event.name != "runtime.ktransformers.phase" && event.name != "runtime.ktransformers.generate") return;

    const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + event.process + ":" + event.thread);
    output_.span({ event.begin_ns, event.end_ns, resource, output_.name(event.name), event.record });
    if (event.name == "runtime.ktransformers.generate") {
        const auto & requests = event.args.at("request_ids");
        if (requests.size() != 1) throw std::runtime_error("Synchronous KTransformers regions require one request");
        output_.request_executions.push_back({
            output_.name(requests.front().get<std::string>()),
            { resource, event.begin_ns, event.end_ns, event.record }
        });
    }
}

} // namespace markov::adapters::ktransformers
