#include "markov/adapters/probes.hpp"

#include <limits>
#include <stdexcept>

namespace markov::adapters {

void ProbeReader::record(std::size_t position, const nlohmann::json & raw) {
    using namespace trace_graph::source;
    const auto phase = raw.at("ph").get<std::string>();
    ++output_.counts["probe_phase." + phase];
    if (phase != "X") return;

    const auto begin = nanoseconds_from_us(raw.at("ts"));
    const auto duration = nanoseconds_from_us(raw.at("dur"));
    if (duration > std::numeric_limits<Time>::max() - begin) throw std::runtime_error("Probe end timestamp overflows nanoseconds");
    const ProbeEvent event{
        .name = raw.at("name").get<std::string>(),
        .args = raw.at("args"),
        .process = scalar_identity(raw.at("pid")),
        .thread = scalar_identity(raw.at("tid")),
        .begin_ns = begin,
        .end_ns = begin + duration,
        .record = { file_id_, position }
    };
    const bool received = event.name == "runtime.request.socket_received";
    const bool sent = event.name == "runtime.response.scheduler_send";
    if (received || sent) {
        const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + event.process + ":" + event.thread);
        const auto point = output_.point({ sent ? event.end_ns : event.begin_ns, resource, output_.name(event.name), event.record });
        for (const auto & identity : event.args.at("request_ids")) {
            auto & ports = requests[identity.get<std::string>()];
            (sent ? ports.sent : ports.received).push_back(point);
        }
    }
    observe(event);
    if (event.args.contains("fact") || event.name.starts_with("runtime.")) ++roles[event.name];
}

} // namespace markov::adapters
