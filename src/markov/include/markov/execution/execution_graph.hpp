#pragma once

#include "markov/contracts/control.hpp"
#include "markov/performance/cost_query.hpp"
#include "markov/performance/cost_requirements.hpp"
#include "markov/performance/node_costs.hpp"
#include "markov/templates/templates.hpp"

#include <nlohmann/json.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace markov {

struct ExecutionTimingGap {
    std::string reason;
    WorkId work;
    std::uint32_t rank;
    std::optional<SourceSpanId> source_span;
    std::optional<std::filesystem::path> source_manifest = std::nullopt;
    nlohmann::json cost_query = nullptr;
};

// Target identities are independent of the source capture and unique across batches.
class ExecutionGraph {
public:
    explicit ExecutionGraph(std::optional<std::filesystem::path> path);
    bool append(std::span<TemplateInstance *> instances, std::span<const InvokeStep> invocations, std::int64_t release_ns);
    std::expected<std::vector<ExecutionEvent>, ExecutionTimingGap> schedule(const NodeCosts & costs, std::span<const InvokeStep> invocations);
    void record_batch(const nlohmann::json & invocations);
    void retain(std::span<const TemplateEndpoint> resources, std::span<const PortRef> ports);
    void release_batch();
    void finish();
    nlohmann::json describe() const;
    const CostRequirements & requirements() const { return requirements_; }

private:
    struct ObservedBoundary {
        BoundaryId source;
        std::size_t source_asset;
    };
    struct Gate {
        WorkId work;
        std::uint32_t rank;
        PortKind kind;

        auto operator<=>(const Gate &) const = default;
    };
    struct RuleBoundary {
        WorkId work;
    };
    struct Boundary {
        std::variant<ObservedBoundary, TemplateEndpoint, Gate, RuleBoundary> origin;
        std::int64_t release_ns;
        const CpuBoundary * cpu = nullptr;
    };
    struct Node {
        WorkId work;
        std::uint32_t rank;
        std::size_t source_asset;
        std::optional<SourceSpanId> source;
        TemplateNodeRole role;
        SourceSpanKind kind;
        const SourceKernel * kernel = nullptr; // Owned by the bank or a runtime template instance.
        BoundaryId begin;
        BoundaryId end;
        const SourceTransfer * transfer = nullptr;          // Owned by the bank or target instance.
        const DeviceCommandWork * device_command = nullptr; // Immutable source observation; endpoint relocation preserves its work.
        const CollectiveWork * collective = nullptr;        // Target logical transport work or the owning context of ordinary CPU work.
        std::optional<CpuServiceWork> cpu = std::nullopt;
        std::optional<NodeCostEstimate> cost = std::nullopt;
        const nlohmann::json * workspace = nullptr;                  // Diagnostic use record, borrowed until this batch is written.
        std::optional<TemplateResource> cpu_resource = std::nullopt; // Diagnostic thread identity, not a new execution resource.
        std::optional<std::size_t> input_slot = std::nullopt;        // Actual incoming handler context, independent of selected batch slots.

        CostQuery cost_query() const;
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
        bool notifies_controller;
    };
    struct InputReady {
        WorkId work;
        std::uint32_t rank;
        BoundaryId boundary;
        std::size_t input_slot;
        RequestId request_id;
        bool notifies_runtime;
    };
    struct Output {
        WorkId work;
        std::uint32_t rank;
        BoundaryId boundary;
        std::size_t request_slot;
        RequestOutput effect;
    };
    struct Unresolved {
        WorkId work;
        std::uint32_t rank;
        std::optional<std::size_t> source_edge;
        std::optional<SourceSpanId> source_span;
    };

    bool acyclic() const;
    const nlohmann::json * workspace_effect(const TemplateInstance & instance, const TemplateNode & node) const;
    void group_cpu_services(std::span<TemplateInstance *> instances, std::span<const InvokeStep> invocations, std::span<const BoundaryId> interfaces);
    BoundaryId resolve(const TemplateEndpoint & endpoint);
    BoundaryId resolve(const Gate & gate);
    void write_batch(const nlohmann::json & invocations);
    std::size_t register_source(const std::filesystem::path & manifest);

    std::vector<std::filesystem::path> sources_;
    std::vector<Boundary> boundaries_;
    std::vector<Node> nodes_;
    // Constructed only for diagnostics and released with the current batch.
    std::vector<nlohmann::json> logical_scopes_;
    std::vector<Edge> edges_;
    std::vector<Port> ports_;
    std::vector<Output> outputs_;
    std::vector<InputReady> inputs_;
    std::optional<ExecutionTimingGap> assembly_gap_;
    std::vector<Unresolved> unresolved_;
    std::map<std::pair<WorkId, std::uint32_t>, std::vector<BoundaryId>> instance_boundaries_;
    std::map<Gate, BoundaryId> gates_;
    std::map<TemplateEndpoint, BoundaryId> resource_imports_;
    std::map<Gate, BoundaryId> gate_imports_;

    // Availability may be in the future: these are immutable scheduled times,
    // not a claim that an asynchronous operation has physically completed.
    std::map<TemplateEndpoint, std::int64_t> resource_times_;
    std::map<Gate, std::int64_t> port_times_;
    std::size_t batches_ = 0;
    bool acyclic_ = true;
    std::vector<std::int64_t> completion_ns_;
    CostRequirements requirements_;

    std::size_t total_boundaries_ = 0;
    std::size_t total_nodes_ = 0;
    std::size_t total_edges_ = 0;
    std::size_t total_ports_ = 0;
    std::size_t peak_boundaries_ = 0;
    std::size_t peak_nodes_ = 0;
    std::size_t unresolved_count_ = 0;
    std::map<std::string, std::size_t> node_roles_;
    std::map<std::string, std::size_t> cost_usage_;
    std::map<std::string, std::map<std::string, std::size_t>> cost_usage_by_work_;
    std::optional<std::filesystem::path> path_;
    std::ofstream diagnostics_;
};

} // namespace markov
