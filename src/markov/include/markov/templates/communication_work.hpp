#pragma once

#include "markov/contracts/control.hpp"
#include "markov/graph/source_graph.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace markov {

enum class CommunicationOperation { AllReduce, AllGather };

struct CommunicationCountDomain {
    std::uint64_t minimum;
    std::uint64_t maximum;
    std::string gap;
    auto operator<=>(const CommunicationCountDomain &) const = default;
};

enum class CommunicationByteExtent { Payload, DestinationPartition };

struct CommunicationTaskBytes {
    std::size_t task;
    CommunicationByteExtent extent;
    auto operator<=>(const CommunicationTaskBytes &) const = default;
};

// A producer-qualified byte relation. Unlisted tasks retain their observed
// control payload; task identity, resources and dependencies never change here.
class CommunicationWorkLayout {
    friend class TemplateAssetCodec;

public:
    CommunicationWorkLayout(CommunicationOperation operation, std::uint32_t element_bytes, std::uint32_t partitions, std::uint64_t alignment,
                            std::vector<CommunicationCountDomain> domains, std::vector<CommunicationTaskBytes> relations)
        : operation_(operation),
          element_bytes_(element_bytes),
          partitions_(partitions),
          alignment_(alignment),
          domains_(std::move(domains)),
          relations_(std::move(relations)) {}
    std::expected<std::vector<SourceCommunicationTask>, std::string> evaluate(std::span<const SourceCommunicationTask> source, std::uint64_t count) const;
    nlohmann::json describe_parameters() const;

    CommunicationOperation operation() const { return operation_; }
    auto operator<=>(const CommunicationWorkLayout &) const = default;

private:
    CommunicationOperation operation_;
    std::uint32_t element_bytes_;
    std::uint32_t partitions_;
    std::uint64_t alignment_;
    std::vector<CommunicationCountDomain> domains_;
    std::vector<CommunicationTaskBytes> relations_;
};

enum class CollectiveRows { Tokens, Requests };

struct CollectiveCountRule {
    CollectiveRows rows;
    std::uint64_t width;
    std::uint64_t count(const BatchInput & input) const;
    auto operator<=>(const CollectiveCountRule &) const = default;
};

// Source scope and native tasks are observations, not part of rule equivalence.
struct CollectiveParameterRule {
    std::optional<CollectiveCountRule> count;
    std::expected<CommunicationWorkLayout, std::string> work;
    std::strong_ordering operator<=>(const CollectiveParameterRule & other) const;
};

} // namespace markov
