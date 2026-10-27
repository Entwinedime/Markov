#pragma once

#include "markov/trace_graph/source/chrome_reader.hpp"
#include "markov/trace_graph/source/observations.hpp"

#include <map>
#include <string>
#include <vector>

namespace markov::adapters::sglang {

struct RequestPorts {
    std::vector<trace_graph::source::Index> received;
    std::vector<trace_graph::source::Index> sent;
};

// Probe observations stay at the framework boundary. They describe work and
// state; their enclosing wall duration is not a second CPU service charge.
class ProbeReader final : public trace_graph::source::RecordConsumer {
public:
    explicit ProbeReader(trace_graph::source::Observations & output) : output_(output) {}
    void set_file(std::size_t file) { file_id_ = file; }
    void record(std::size_t position, const nlohmann::json & event) override;

    std::map<std::string, RequestPorts> requests;
    std::map<std::string, std::size_t> roles;

private:
    void observe_linear(const nlohmann::json & args, const std::string & process, trace_graph::source::Index resource, trace_graph::source::Time begin,
                        trace_graph::source::Time end, trace_graph::source::RecordRef record);
    trace_graph::source::Observations & output_;
    std::size_t file_id_ = 0;
    std::map<std::string, std::string> forward_phases_;
};

} // namespace markov::adapters::sglang
