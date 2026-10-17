#pragma once

#include "markov/contracts/service_work.hpp"
#include "markov/performance/cost_requirements.hpp"
#include "markov/performance/node_costs.hpp"
#include "markov/templates/templates.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace markov {

struct ExecutionTimingGap {
    std::string reason;
    WorkId work;
    std::uint32_t rank;
    std::optional<SourceSpanId> source_span;
    std::optional<std::filesystem::path> source_manifest = std::nullopt;
    nlohmann::json service_work = nullptr;
};

// Target identities are independent of the source capture and unique across batches.
class ExecutionGraph {
public:
    bool append(std::span<TemplateInstance *> instances, std::span<const InvokeStep> invocations, std::int64_t release_ns);
    std::expected<std::vector<ControlEvent>, ExecutionTimingGap> schedule(const NodeCosts & costs, const std::map<WorkId, const InvokeStep *> & invocations);
    nlohmann::json describe() const;
    nlohmann::json performance_requirements() const { return requirements_.describe(); }
    void write(const std::filesystem::path & path) const;

private:
    struct Boundary {
        BoundaryId source;
        std::size_t source_asset;
        std::size_t batch;
        std::int64_t release_ns;
    };
    struct Node {
        WorkId work;
        std::uint32_t rank;
        std::size_t source_asset;
        SourceSpanId source;
        TemplateNodeRole role;
        SourceSpanKind kind;
        const SourceKernel * kernel;                   // Owned by the bank or a runtime template instance.
        const SourceCommunicationTask * communication; // Owned by the source definition or runtime instance.
        BoundaryId begin;
        BoundaryId end;
        const SourceTransfer * transfer;                    // Owned by the bank or target instance.
        const DeviceCommandWork * device_command = nullptr; // Immutable source observation; endpoint relocation preserves its work.
        std::optional<NodeCostEstimate> cost = std::nullopt;

        ServiceWork service_work() const;
    };
    struct Edge {
        BoundaryId source;
        BoundaryId target;
        std::optional<SourceRelation> relation; // Null identifies a controller dependency.
    };
    struct Port {
        WorkId work;
        std::uint32_t rank;
        PortKind kind;
        BoundaryId boundary;
    };
    struct Unresolved {
        WorkId work;
        std::uint32_t rank;
        std::optional<std::size_t> source_edge;
        std::optional<SourceSpanId> source_span;
    };

    bool acyclic() const;
    void record_requirement(const CostQuery & query, const NodeCosts & costs, const Node & owner, const InvokeStep & invocation);
    BoundaryId resolve(const TemplateEndpoint & endpoint) const;
    std::size_t register_source(const std::filesystem::path & manifest);

    std::vector<std::filesystem::path> sources_;
    std::vector<Boundary> boundaries_;
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    std::vector<Port> ports_;
    std::vector<Unresolved> unresolved_;
    std::map<std::pair<WorkId, std::uint32_t>, std::vector<BoundaryId>> instance_boundaries_;
    std::size_t batches_ = 0;
    bool acyclic_ = true;
    std::size_t pending_port_ = 0;
    std::vector<std::int64_t> completion_ns_;
    CostRequirements requirements_;
    std::size_t requirement_frontier_ = 0;
};

} // namespace markov
