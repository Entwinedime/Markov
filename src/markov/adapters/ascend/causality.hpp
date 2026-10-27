#pragma once

#include "markov/trace_graph/source/chrome_reader.hpp"
#include "markov/trace_graph/source/observations.hpp"

#include <optional>
#include <string>
#include <vector>

namespace markov::adapters::ascend {

// Hook arguments identify reused native events; CANN connection IDs identify
// their device commands. Neither timestamp proximity nor a handle alone suffices.
class RuntimeCausality {
public:
    void observe(const nlohmann::json & event, trace_graph::source::ObservedSpan & span, bool device, bool hook);
    void connect(trace_graph::source::Observations & observations);

private:
    enum class Kind { Record, Wait, EventSynchronize, StreamSynchronize };
    struct Call {
        Kind kind;
        trace_graph::source::ObservedSpan span;
        std::string process;
        std::string identity;
        std::string stream;
    };
    struct Binding {
        const Call * hook;
        const Call * host;
        const Call * device;
    };

    static std::optional<Kind> host_kind(const std::string & name);
    static trace_graph::source::Index port(trace_graph::source::Observations & observations, const Call & call, bool completion);
    std::vector<Binding> bind_calls(trace_graph::source::Observations & observations) const;
    void connect_events(trace_graph::source::Observations & observations, const std::vector<Binding> & bindings) const;
    void connect_streams(trace_graph::source::Observations & observations, const std::vector<Binding> & bindings) const;
    std::vector<Call> hook_calls_;
    std::vector<Call> host_calls_;
    std::vector<Call> device_calls_;
};

} // namespace markov::adapters::ascend
