#include "markov/trace_graph/source/cpu_costs.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace markov::trace_graph::source {

void CpuCorrection::add(CpuCorrection other) {
    if (other.covered_ns > std::numeric_limits<Time>::max() - covered_ns || other.cost_ns > std::numeric_limits<Time>::max() - cost_ns)
        throw std::overflow_error("CPU correction exceeds the clock range");
    covered_ns += other.covered_ns;
    cost_ns += other.cost_ns;
}

CpuCosts::CpuCosts(std::map<Index, std::vector<CpuCostSpan>> lanes) : lanes_(std::move(lanes)) {
    for (auto & [resource, spans] : lanes_) {
        std::ranges::sort(spans, {}, &CpuCostSpan::begin_ns);
        Time previous_end = 0;
        for (const auto & span : spans) {
            if (span.begin_ns < previous_end || span.end_ns <= span.begin_ns || span.cost_ns < 0)
                throw std::runtime_error("CPU correction intervals must be nonnegative, nonempty and disjoint");
            previous_end = span.end_ns;
        }
        spans_ += spans.size();
    }
}

CpuCorrection CpuCosts::interval(Index resource, Time begin_ns, Time end_ns) const {
    CpuCorrection result;
    const auto found = lanes_.find(resource);
    if (found == lanes_.end() || begin_ns == end_ns) return result;

    auto at = std::ranges::upper_bound(found->second, begin_ns, {}, &CpuCostSpan::end_ns);
    for (; at != found->second.end() && at->begin_ns < end_ns; ++at) {
        const auto begin = std::max(begin_ns, at->begin_ns), end = std::min(end_ns, at->end_ns);

        // Cumulative rounding makes any partition preserve the same total.
        // Within a measured budget the allocation is proportional, not a new measurement.
        const auto cumulative = [&](Time time) -> Time {
            if (time == at->end_ns) return at->cost_ns;
            return static_cast<Time>(static_cast<long double>(at->cost_ns) * (time - at->begin_ns) / (at->end_ns - at->begin_ns));
        };
        result.add({ end - begin, cumulative(end) - cumulative(begin) });
    }
    return result;
}

} // namespace markov::trace_graph::source
