#pragma once

#include "markov/trace_graph/source/observations.hpp"

#include <optional>
#include <vector>

namespace markov::trace_graph::source {

class SourceContext;

struct FrameRegions {
    std::optional<Index> before_preparation, before_run, before_result;
};

struct ResidualRegion {
    ObservedRegion observation;
    std::vector<ObservedRegion> excluded;
};

// Source semantic boundaries and residual references. This does not construct
// native event nodes or estimate the cost of changed target work.
class Regions {
public:
    explicit Regions(const SourceContext & source);
    const FrameRegions & frame(Index id) const { return frames_[id]; }
    const ResidualRegion & residual(Index id) const { return residuals_[id]; }
    std::size_t residual_count() const { return residuals_.size(); }

private:
    std::vector<FrameRegions> frames_;
    std::vector<ResidualRegion> residuals_;
};

} // namespace markov::trace_graph::source
