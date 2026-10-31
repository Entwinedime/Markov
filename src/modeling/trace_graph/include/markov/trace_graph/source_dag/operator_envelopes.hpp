#pragma once

#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/core/trace_event.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace markov::trace_graph::source_dag::detail {

// Shared by modeled source call views. Preserve submicrosecond
// boundaries rather than letting rounded envelopes claim nearby submissions.
class OperatorEnvelopeIndex {
public:
    explicit OperatorEnvelopeIndex(const std::vector<core::TraceEvent> & events) {
        for (size_t i = 0; i < events.size(); ++i) {
            const auto & event = events[i];
            if ((event.source_channel == core::TraceSourceChannel::Torch && event.cat == "cpu_op" && event.ph == 'X')
                || (event.source_channel == core::TraceSourceChannel::PythonProbe
                    && ((event.name == "runtime.phase.kernel" && event.arg("status") == "returned") || event.name == "runtime.triton.prepare"
                        || event.name == "runtime.triton.load")))
                lanes_[{ event.pid, event.tid }].push_back({ i, begin_ns(event), end_ns(event) });
        }
        for (auto & [lane, envelopes] : lanes_) std::ranges::stable_sort(envelopes, {}, &Envelope::begin_ns);
    }

    [[nodiscard]] std::vector<size_t> inside(const core::TraceEvent & call) const {
        const auto lane = lanes_.find({ call.pid, call.tid });
        if (lane == lanes_.end()) return {};

        std::vector<size_t> result;
        const auto finish = end_ns(call);
        auto op = std::ranges::lower_bound(lane->second, begin_ns(call), {}, &Envelope::begin_ns);
        for (; op != lane->second.end() && op->begin_ns < finish; ++op)
            if (op->end_ns <= finish) result.push_back(op->observation);
        return result;
    }

private:
    static uint64_t begin_ns(const core::TraceEvent & event) {
        return core::checked_add_u64(core::checked_multiply_u64(event.ts, 1'000, "operator timestamp overflow"),
                                     event.ts_submicro_ns,
                                     "operator timestamp overflow");
    }

    static uint64_t end_ns(const core::TraceEvent & event) {
        const auto duration = core::checked_add_u64(core::checked_multiply_u64(event.dur, 1'000, "operator duration overflow"),
                                                    event.dur_submicro_ns,
                                                    "operator duration overflow");
        return core::checked_add_u64(begin_ns(event), duration, "operator end timestamp overflow");
    }

    struct Envelope {
        size_t observation;
        uint64_t begin_ns, end_ns;
    };
    std::map<std::pair<std::string, std::string>, std::vector<Envelope>> lanes_;
};

} // namespace markov::trace_graph::source_dag::detail
