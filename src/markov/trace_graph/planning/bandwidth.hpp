#pragma once

#include "markov/trace_graph/planning/program.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

namespace markov::trace_graph::planning {

enum class TransferDirection { DeviceToHost, HostToDevice, HostToStorage, StorageToHost };

struct BandwidthLimits {
    // PCIe directions are per rank; file directions and their common limit are group-wide.
    std::array<double, 4> bytes_per_sec;
    double storage_shared_bytes_per_sec;
};

class Bandwidth {
public:
    Bandwidth(BandwidthLimits limits, std::size_t participants, std::size_t resource_begin);
    std::size_t resources() const { return tails_.size(); }
    graph::NodeId admit(PlannedMutation & mutation, graph::NodeId begin, TransferDirection direction, std::size_t bytes, std::size_t rank,
                        std::optional<graph::NodeId> cause);

private:
    BandwidthLimits limits_;
    std::size_t participants_, resource_begin_;
    std::vector<std::optional<graph::NodeId>> tails_;
};

} // namespace markov::trace_graph::planning
