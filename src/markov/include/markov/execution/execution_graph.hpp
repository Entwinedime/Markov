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
    std::expected<std::vector<ControlEvent>, ExecutionTimingGap> schedule(const NodeCosts & costs, std::span<const InvokeStep> invocations);
    nlohmann::json describe() const;
    nlohmann::json performance_requirements(bool diagnostics) const { return requirements_.describe(diagnostics); }
    void write(const std::filesystem::path & path, const nlohmann::json & invocations) const;

private:
    struct Boundary {
        BoundaryId source;
        std::size_t source_asset;
        std::size_t batch;
        std::int64_t release_ns;
        const CpuBoundary * cpu = nullptr;
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
        std::optional<CpuServiceWork> cpu = std::nullopt;
        std::vector<std::size_t> cpu_members = {};
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
    void group_cpu_services(std::size_t first_node, std::span<const InvokeStep> invocations, std::span<const BoundaryId> interfaces);
    void record_requirement(const CostQuery & query, const NodeCostResult & measured, const Node & owner, const InvokeStep & invocation);
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
};

} // namespace markov
