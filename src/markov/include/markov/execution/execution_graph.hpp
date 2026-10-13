#pragma once

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
        const SourceTransfer * transfer;               // Owned by the bank or target instance.
        const PublicationWork * publication = nullptr; // Instance-owned target output or member work.
        std::optional<std::size_t> publication_member_slot = std::nullopt;
        std::optional<SourceCpuPart> control_part = std::nullopt;
        std::optional<std::size_t> admission_member_slot = std::nullopt;
        std::optional<CpuSubmissionWork> cpu_submission = std::nullopt;
        std::optional<NodeCostEstimate> cost = std::nullopt;
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
    void bind_cpu_submissions(std::size_t first_node);
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
};

} // namespace markov
