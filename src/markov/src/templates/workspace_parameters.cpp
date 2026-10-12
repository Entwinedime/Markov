#include "markov/templates/workspace_parameters.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace markov {

WorkspaceDemandLayout::WorkspaceDemandLayout(std::uint32_t units, std::uint64_t bytes_per_unit, std::uint32_t alignment,
                                             std::optional<WorkspacePartition> partition, std::vector<WorkspaceScaledBuffer> buffers)
    : units_(units),
      bytes_per_unit_(bytes_per_unit),
      alignment_(alignment),
      partition_(std::move(partition)),
      buffers_(std::move(buffers)) {}

std::expected<WorkspaceDemand, std::string> WorkspaceDemandLayout::evaluate(const BatchInput & input) const {
    auto units = units_;
    if (partition_) {
        const auto & partition = *partition_;
        const auto members = input.requests.size();
        if (members < partition.minimum_members) return std::unexpected("workspace_partition_member_domain");
        for (const auto & request : input.requests) {
            if (request.input_tokens != 1) return std::unexpected("workspace_partition_query_domain");
            if (std::uint64_t(request.input_tokens) + request.context_tokens >= partition.maximum_sequence_exclusive)
                return std::unexpected("workspace_partition_sequence_domain");
        }

        const auto units_per_member = (units + members - 1) / members;
        auto group_size = std::min<std::uint64_t>((partition.groups + units_per_member - 1) / units_per_member, partition.group_size_limit);
        if (const auto override = partition.group_size_by_members.find(members); override != partition.group_size_by_members.end())
            group_size = override->second;
        const auto member_units = (partition.groups + group_size - 1) / group_size;
        units = static_cast<std::uint32_t>(std::min<std::uint64_t>(units, members * member_units));
    }

    std::uint64_t bytes = units * bytes_per_unit_;
    for (const auto & buffer : buffers_) {
        // Each intermediate is float32, followed by the native uint32 cast.
        float extent = static_cast<float>(units) * buffer.scale;
        extent *= static_cast<float>(buffer.groups);
        extent *= static_cast<float>(units);
        extent *= static_cast<float>(buffer.element_bytes);
        if (extent >= static_cast<float>(std::numeric_limits<std::uint32_t>::max())) return std::unexpected("workspace_buffer_extent_out_of_range");
        bytes += static_cast<std::uint32_t>(extent);
    }
    bytes = (bytes + alignment_ - 1) / alignment_ * alignment_;
    return WorkspaceDemand{ units, bytes };
}

} // namespace markov
