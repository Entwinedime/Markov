#pragma once

#include "markov/trace_graph/source/observations.hpp"

#include <cstddef>
#include <map>
#include <vector>

namespace markov::trace_graph::source {

struct CpuCostSpan {
    Time begin_ns, end_ns, cost_ns;
};

struct CpuCorrection {
    Time covered_ns = 0, cost_ns = 0;
    void add(CpuCorrection other);
};

// Estimated costs stay on original source coordinates. A covered interval is
// not proof that its wall time is CPU service or that its work can migrate.
class CpuCosts {
public:
    explicit CpuCosts(std::map<Index, std::vector<CpuCostSpan>> lanes);
    CpuCorrection interval(Index resource, Time begin_ns, Time end_ns) const;
    std::size_t spans() const { return spans_; }

private:
    std::map<Index, std::vector<CpuCostSpan>> lanes_;
    std::size_t spans_ = 0;
};

} // namespace markov::trace_graph::source
