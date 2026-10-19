#pragma once

#include "markov/graph/source_graph.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace markov {

// The same structural partition serves source inventories and target execution.
// Owners distinguish serving actions; neither source clocks nor costs select cuts.
class CpuServicePartition {
public:
    struct Segment {
        std::size_t index;
        BoundaryId begin;
        BoundaryId end;
        std::uint64_t owner;
        std::uint32_t rank;
    };
    struct Unit {
        BoundaryId begin;
        BoundaryId end;
        CpuServiceDescriptor service;
        std::vector<std::size_t> members;
    };

    explicit CpuServicePartition(std::size_t boundaries) : names_(boundaries) {}

    void name(BoundaryId point, const CpuBoundary * boundary) { names_[point] = boundary; }
    void protect(BoundaryId point) { cuts_.insert(point); }
    void connect(BoundaryId source, BoundaryId target, std::optional<SourceRelation> relation);
    void wait(BoundaryId begin, BoundaryId end);
    void add(Segment segment);
    std::vector<Unit> units() const;

private:
    bool is_cut(BoundaryId point) const;

    std::vector<const CpuBoundary *> names_;
    std::multimap<BoundaryId, BoundaryId> next_;
    std::set<BoundaryId> cuts_;
    std::vector<Segment> segments_;
    std::map<BoundaryId, std::size_t> starts_;
};

} // namespace markov
