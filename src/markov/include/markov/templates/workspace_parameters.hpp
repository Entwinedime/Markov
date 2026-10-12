#pragma once

#include "markov/contracts/control.hpp"

#include <compare>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace markov {

struct WorkspacePartition {
    std::uint32_t groups;
    std::uint32_t group_size_limit;
    std::uint32_t minimum_members;
    std::uint32_t maximum_sequence_exclusive;
    std::map<std::uint32_t, std::uint32_t> group_size_by_members;
    auto operator<=>(const WorkspacePartition &) const = default;
};

// Preserve a producer's float32 multiplication and truncation boundary.
// Folding this into a fitted coefficient changes native allocation thresholds.
struct WorkspaceScaledBuffer {
    float scale;
    std::uint32_t groups;
    std::uint32_t element_bytes;
    auto operator<=>(const WorkspaceScaledBuffer &) const = default;
};

struct WorkspaceDemand {
    std::uint32_t work_units;
    std::uint64_t bytes;
};

class WorkspaceDemandLayout {
    friend class TemplateAssetCodec;

public:
    WorkspaceDemandLayout(std::uint32_t units, std::uint64_t bytes_per_unit, std::uint32_t alignment, std::optional<WorkspacePartition> partition,
                          std::vector<WorkspaceScaledBuffer> buffers);
    std::expected<WorkspaceDemand, std::string> evaluate(const BatchInput & input) const;
    auto operator<=>(const WorkspaceDemandLayout &) const = default;

private:
    std::uint32_t units_;
    std::uint64_t bytes_per_unit_;
    std::uint32_t alignment_;
    std::optional<WorkspacePartition> partition_;
    std::vector<WorkspaceScaledBuffer> buffers_;
};

} // namespace markov
