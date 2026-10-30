#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace markov::trace_graph::core {

/** Normal CPU service on original trace coordinates. No timing or dependency edits.
 * Subranges use cumulative integer costs, so splitting preserves the total. */
class CpuServiceCost {
public:
    struct Span {
        uint64_t begin_us, end_us, service_us;
    };
    using Lane = std::pair<std::string, std::string>; // Original pid/tid.

    void add(Lane lane, Span span) {
        if (span.end_us <= span.begin_us) throw std::invalid_argument("CPU service span is empty");
        auto & spans = own_lanes()[std::move(lane)];
        const auto at = std::lower_bound(spans.begin(), spans.end(), span.begin_us, [](const Span & s, uint64_t begin) { return s.begin_us < begin; });
        if ((at != spans.end() && at->begin_us < span.end_us) || (at != spans.begin() && std::prev(at)->end_us > span.begin_us))
            throw std::invalid_argument("CPU service spans overlap");
        spans.insert(at, span);
    }

    /** Merge an independently prepared batch without repeatedly shifting the
     * existing lane. Input and existing intervals must remain nonoverlapping;
     * sorting changes insertion order only, never coordinates or costs. */
    void add_spans(Lane lane, std::vector<Span> spans) {
        const auto by_begin = [](const Span & left, const Span & right) { return left.begin_us < right.begin_us; };
        if (!std::ranges::is_sorted(spans, by_begin)) std::ranges::sort(spans, by_begin);
        auto & current = own_lanes()[std::move(lane)];
        if (!current.empty()) {
            std::vector<Span> merged;
            merged.reserve(current.size() + spans.size());
            std::merge(current.begin(), current.end(), spans.begin(), spans.end(), std::back_inserter(merged), by_begin);
            spans = std::move(merged);
        }

        for (size_t index = 0; index < spans.size(); ++index) {
            if (spans[index].end_us <= spans[index].begin_us) throw std::invalid_argument("CPU service span is empty");
            if (index && spans[index - 1].end_us > spans[index].begin_us) throw std::invalid_argument("CPU service spans overlap");
        }
        current = std::move(spans);
    }

    [[nodiscard]] uint64_t duration(const Lane & lane, uint64_t begin, uint64_t end) const {
        if (end < begin) throw std::invalid_argument("CPU service interval is reversed");
        const auto found = lanes_->find(lane);
        if (found == lanes_->end()) return end - begin;
        const auto & spans = found->second;
        auto at = std::lower_bound(spans.begin(), spans.end(), begin, [](const Span & s, uint64_t value) { return s.end_us <= value; });
        __int128 result = end - begin;
        for (; at != spans.end() && at->begin_us < end; ++at) {
            const auto lo = std::max(begin, at->begin_us), hi = std::min(end, at->end_us);
            const auto cumulative = [&](uint64_t time) { return static_cast<__uint128_t>(time - at->begin_us) * at->service_us / (at->end_us - at->begin_us); };
            result += static_cast<__int128>(cumulative(hi) - cumulative(lo)) - (hi - lo);
        }
        if (result < 0 || result > std::numeric_limits<uint64_t>::max()) throw std::overflow_error("CPU service duration exceeds uint64 range");
        return static_cast<uint64_t>(result);
    }

    [[nodiscard]] bool empty() const { return lanes_->empty(); }
    [[nodiscard]] const auto & lanes() const { return *lanes_; }

private:
    using Lanes = std::map<Lane, std::vector<Span>>;
    Lanes & own_lanes() {
        if (!lanes_.unique()) lanes_ = std::make_shared<Lanes>(*lanes_);
        return *lanes_;
    }
    std::shared_ptr<Lanes> lanes_ = std::make_shared<Lanes>();
};

} // namespace markov::trace_graph::core
