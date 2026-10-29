#pragma once

#include "markov/trace_graph/source/chrome_reader.hpp"
#include "markov/trace_graph/source/observations.hpp"

#include <map>
#include <string>
#include <vector>

namespace markov::adapters {

struct RequestPorts {
    std::vector<trace_graph::source::Index> received;
    std::vector<trace_graph::source::Index> sent;
};

// Borrowed only during record interpretation. Framework readers retain typed
// facts and original record references, never views into the temporary JSON.
struct ProbeEvent {
    std::string name;
    const nlohmann::json & args;
    std::string process;
    std::string thread;
    trace_graph::source::Time begin_ns;
    trace_graph::source::Time end_ns;
    trace_graph::source::RecordRef record;
};

// The request protocol is shared by supported frameworks. Specialized readers
// add their work/state facts without copying request or timestamp interpretation.
class ProbeReader : public trace_graph::source::RecordConsumer {
public:
    explicit ProbeReader(trace_graph::source::Observations & output) : output_(output) {}
    void set_file(std::size_t file) { file_id_ = file; }
    void record(std::size_t position, const nlohmann::json & event) final;
    virtual void finish() {}

    std::map<std::string, RequestPorts> requests;
    std::map<std::string, std::size_t> roles;

protected:
    virtual void observe(const ProbeEvent &) {}
    trace_graph::source::Observations & output_;

private:
    std::size_t file_id_ = 0;
};

} // namespace markov::adapters
