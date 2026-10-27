#pragma once

#include "markov/adapters/probes.hpp"

#include <map>
#include <string>
#include <vector>

namespace markov::adapters::sglang {

// Probe observations stay at the framework boundary. They describe work and
// state; their enclosing wall duration is not a second CPU service charge.
class ProbeReader final : public adapters::ProbeReader {
public:
    using adapters::ProbeReader::ProbeReader;

private:
    void observe(const ProbeEvent & event) override;
    trace_graph::planning::PrefixId token_path(const nlohmann::json & dictionary);
    void observe_linear(const nlohmann::json & args, const std::string & process, trace_graph::source::Index resource, trace_graph::source::Time begin,
                        trace_graph::source::Time end, trace_graph::source::RecordRef record);
    std::map<std::string, std::string> forward_phases_;
    std::map<std::string, trace_graph::planning::PrefixId> token_descriptors_;
};

} // namespace markov::adapters::sglang
