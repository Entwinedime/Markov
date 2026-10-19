#include "markov/graph/cpu_service_partition.hpp"

#include <utility>

namespace markov {

void CpuServicePartition::connect(BoundaryId source, BoundaryId target, std::optional<SourceRelation> relation) {
    if (relation == SourceRelation::ThreadOrder) next_.emplace(source, target);
    else if (relation != SourceRelation::IntervalExtent) {
        protect(source);
        protect(target);
    }
}

void CpuServicePartition::add(Segment segment) {
    starts_.emplace(segment.begin, segments_.size());
    segments_.push_back(segment);
}

void CpuServicePartition::wait(BoundaryId begin, BoundaryId end) {
    // Readiness can be priced across an unobservable SDK wait. This removes
    // only cost cuts; execution retains the device condition in the same DAG.
    for (const auto point : { begin, end }) {
        if (names_[point] && names_[point]->kind == CpuBoundaryKind::InternalWait) cuts_.erase(point);
        else protect(point);
    }
}

bool CpuServicePartition::is_cut(BoundaryId point) const {
    if (cuts_.contains(point)) return true;
    const auto * boundary = names_[point];
    if (!boundary) return false;

    switch (boundary->kind) {
    case CpuBoundaryKind::Scope:
    case CpuBoundaryKind::Worker:
    case CpuBoundaryKind::Wait:
    case CpuBoundaryKind::Transfer:
        return true;
    case CpuBoundaryKind::Enqueue:
    case CpuBoundaryKind::Runtime:
        return boundary->edge == CpuBoundaryEdge::Begin;
    case CpuBoundaryKind::Function:
    case CpuBoundaryKind::InternalWait:
        return false;
    }
    std::unreachable();
}

std::vector<CpuServicePartition::Unit> CpuServicePartition::units() const {
    std::vector<std::optional<std::size_t>> following(segments_.size());
    std::vector<std::optional<BoundaryId>> terminal(segments_.size());
    std::vector<std::size_t> predecessors(segments_.size());
    for (const auto & [begin, index] : starts_) {
        const auto & segment = segments_[index];
        auto point = segment.end;
        // A function or probe envelope supplies a name, not an execution
        // interface. Private CPU helpers remain inside the surrounding unit.
        while (!is_cut(point)) {
            const auto candidate = starts_.find(point);
            if (candidate != starts_.end()) {
                const auto & next = segments_[candidate->second];
                if (next.owner == segment.owner && next.rank == segment.rank) {
                    following[index] = candidate->second;
                    ++predecessors[candidate->second];
                }
                break;
            }

            // FIFO edges to another serving action constrain execution, not
            // the continuation of this action's measurable CPU interval.
            const auto [first, final] = next_.equal_range(point);
            auto continuation = next_.end();
            for (auto order = first; order != final; ++order) {
                const auto target = starts_.find(order->second);
                if (target != starts_.end()) {
                    const auto & next = segments_[target->second];
                    if (next.owner != segment.owner || next.rank != segment.rank) continue;
                }

                if (continuation != next_.end() && continuation->second != order->second) {
                    continuation = next_.end();
                    break;
                }
                continuation = order;
            }

            if (continuation == next_.end()) break;
            point = continuation->second;
        }

        // A temporal observation hole has no price. Its named endpoint can
        // close the preceding service without inventing additional work.
        if (names_[point]) terminal[index] = point;
    }

    // A shared successor is a join, not one serial service interval.
    std::vector<bool> has_predecessor(segments_.size());
    for (auto & next : following) {
        if (!next) continue;
        if (predecessors[*next] != 1) next.reset();
        else has_predecessor[*next] = true;
    }

    std::vector<Unit> result;
    for (const auto & [begin, index] : starts_) {
        if (has_predecessor[index]) continue;

        std::vector<std::size_t> members{ segments_[index].index };
        auto last = index;
        while (following[last]) {
            last = *following[last];
            members.push_back(segments_[last].index);
        }

        const auto end = terminal[last].value_or(segments_[last].end);
        const auto * first = names_[begin];
        const auto * final = names_[end];
        if (first && final && first->role == final->role)
            result.push_back({
                begin,
                end,
                { *first, *final },
                std::move(members)
            });
    }
    return result;
}

} // namespace markov
