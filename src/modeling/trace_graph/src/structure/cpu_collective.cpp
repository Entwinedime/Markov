#include "markov/trace_graph/structure/cpu_collective.hpp"

#include <bit>

namespace markov::trace_graph::structure {

CpuCollectiveEntries cpu_collective_entries(CpuCollectiveKind kind, std::span<const int> members, size_t root_index) {
    CpuCollectiveEntries result;
    const auto size = members.size();
    for (size_t member = 0; member < size; ++member) {
        auto & entries = result[members[member]];
        if (kind == CpuCollectiveKind::AllReduce) {
            entries.assign(members.begin(), members.end());
            continue;
        }

        const auto rank = (member + size - root_index) % size;
        const auto actual = [&](size_t relative) { return members[(relative + root_index) % size]; };
        entries.push_back(members[member]);
        for (size_t ancestor = rank; ancestor != 0;) {
            ancestor ^= std::bit_floor(ancestor);
            entries.push_back(actual(ancestor));
        }
        for (size_t bit = 1; bit < size; bit <<= 1)
            if (rank < bit && rank + bit < size) entries.push_back(actual(rank + bit));
    }
    return result;
}

} // namespace markov::trace_graph::structure
