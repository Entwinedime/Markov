#pragma once

#include "markov/contracts/control.hpp"
#include "markov/contracts/environment.hpp"
#include "markov/contracts/service_work.hpp"
#include "markov/contracts/source_observations.hpp"
#include "markov/graph/source_graph.hpp"
#include "markov/templates/attention_parameters.hpp"
#include "markov/templates/batch_parameters.hpp"
#include "markov/templates/communication_work.hpp"
#include "markov/templates/kv_parameters.hpp"
#include "markov/templates/model_parameters.hpp"
#include "markov/templates/publication_parameters.hpp"
#include "markov/templates/request_pool_parameters.hpp"
#include "markov/templates/transfer_parameters.hpp"
#include "markov/templates/workspace_parameters.hpp"

#include <nlohmann/json.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace markov {

enum class TemplateNodeRole { Service, Wait, Annotation };

enum class TemplateResourceRole { Control, Submission, Model, Collective };

struct TemplateResource {
    SourceDomain domain;
    std::uint32_t rank;
    std::uint64_t native_id;
    std::optional<TemplateResourceRole> role = std::nullopt;

    // Native IDs remain provenance when a producer has qualified a common role.
    auto key() const { return std::tuple{ domain, rank, role, role ? 0 : native_id }; }
    auto operator<=>(const TemplateResource & other) const { return key() <=> other.key(); }
    bool operator==(const TemplateResource & other) const { return key() == other.key(); }

    nlohmann::json describe() const;
};

// Allocation observations are bound to operations and resources during production.
// Initial capacity belongs to the source pool, never to individual model layers.
struct TemplateWorkspacePool {
    TemplateResource resource;
    std::uint64_t initial_capacity_bytes;
    std::uint32_t allocation_padding_bytes;
    std::uint32_t allocation_alignment_bytes;
};

enum class WorkspaceBranch { Reuse, Allocate, Grow, Release };

struct TemplateWorkspaceUse {
    std::size_t pool;
    std::size_t operation;
    std::size_t worker;
    std::size_t source_api;
    SourceSpanId marker;
    std::size_t begin; // Local allocator observation boundaries, inside its worker operation.
    std::size_t end;
    std::uint64_t requested_bytes;
    std::uint64_t capacity_before_bytes;
    std::uint64_t capacity_after_bytes;
    WorkspaceBranch branch;
    bool requires_device_synchronization;
    // A qualified rule borrows the source owner; otherwise retain the producer's reason.
    std::expected<const WorkspaceDemandLayout *, std::string> demand = std::unexpected("workspace_demand_binding_not_qualified");
};

struct TemplateLane {
    std::size_t source_lane;
    SourceLane source;
    std::optional<TemplateResourceRole> role = std::nullopt;

    TemplateResource resource() const { return { source.domain, source.rank, source.native_id, role }; }
};

// Local structure contains no source identity or observed clock coordinate.
struct TemplateBoundary {
    std::size_t lane;
    auto operator<=>(const TemplateBoundary &) const = default;
};

struct TemplateBoundaryObservation {
    BoundaryId source_boundary;
    std::int64_t observed_time_ns; // Source CPU ordering only, never a target duration.
};

// Insertions receive target identities; donor identity remains diagnostic provenance.
struct TemplateInsertedBoundary {
    TemplateBoundary boundary;
    BoundaryId source_boundary;
    const std::filesystem::path * source_manifest;
};

struct TemplateDefinition;

struct TemplateRegionReference {
    const TemplateDefinition * definition;
    std::size_t region;
};

struct TemplateStreamWait {
    SourceSpanId source_span;
    std::optional<std::uint64_t> native_stream;
    std::size_t completion;
    std::vector<TemplateResource> resources;
    SynchronizationScope scope = SynchronizationScope::Stream;

    bool has_resource_scope() const { return !resources.empty() && (scope == SynchronizationScope::Device || native_stream.has_value()); }
};

// Structural slots are local to a definition, never native source identities.
struct TemplateNode {
    std::size_t observation_slot;
    SourceSpanKind kind;
    std::size_t begin;
    std::size_t end;

    TemplateNodeRole role() const;
    auto operator<=>(const TemplateNode &) const = default;
};

// Local topology is independent of the observation that supplied its endpoints.
struct TemplateDependency {
    SourceRelation relation;
    std::size_t source;
    std::size_t target;
    auto operator<=>(const TemplateDependency &) const = default;
};

enum class TemplateConnectionDirection { Incoming, Outgoing };

struct TemplateConnectionContract {
    SourceRelation relation;
    TemplateConnectionDirection direction;
    std::size_t local_boundary;
    auto operator<=>(const TemplateConnectionContract &) const = default;
};

// One interface cut, within a serving definition or across definitions.
// Remote slots preserve aliases without assigning outside work to this region.
struct TemplateInterface {
    TemplateConnectionContract connection;
    std::size_t remote_boundary;
    auto operator<=>(const TemplateInterface &) const = default;
};

struct TemplateConnectionEndpoint {
    TemplateConnectionContract contract;
    BoundaryId remote_source_boundary;
};

struct TemplateResourceSlot {
    SourceDomain domain;
    std::size_t rank; // Zero is this definition's rank; peer slots preserve rank aliases.
    auto operator<=>(const TemplateResourceSlot &) const = default;
};

struct TemplateRemoteBoundary {
    std::size_t lane;
    std::optional<std::size_t> local_boundary; // Alias to a region-local endpoint, when present.
    auto operator<=>(const TemplateRemoteBoundary &) const = default;
};

// Local resource slots and qualified model rules belong to the shared graph.
// Actual lanes and peer identities remain bindings; sharing does not qualify a target.
struct TemplateRegionStructure {
    std::vector<TemplateResourceSlot> resources;
    std::vector<TemplateBoundary> boundaries; // Lane indexes a local resource slot.
    std::vector<TemplateNode> nodes;
    std::vector<TemplateDependency> dependencies;
    std::vector<TemplateInterface> connections;
    std::vector<TemplateInterface> dependency_ports;
    std::vector<TemplateRemoteBoundary> remote_boundaries; // Shares resource slots with local boundaries.
    std::map<std::size_t, const CollectiveParameterRule *> collective_rules;
    std::map<std::size_t, const ModelParameterRule *> model_rules; // Source-owned canonical rules, indexed by local node.
    auto operator<=>(const TemplateRegionStructure &) const = default;
};

using TemplateRegionStructures = std::set<TemplateRegionStructure>;

// Iteration addresses source node slots; dependencies address source edge slots.
// Node and dependency views restore endpoints through the same region binding.
// Boundary reads resolve local resource slots to the selected observation lanes.
class TemplateGraph {
    friend class TemplateAssetCodec;

public:
    struct Iterator {
        using iterator_category = std::forward_iterator_tag;
        using value_type = TemplateNode;
        using difference_type = std::ptrdiff_t;
        using reference = TemplateNode;
        using pointer = void;

        const TemplateGraph * owner;
        std::size_t position;

        TemplateNode operator*() const { return (*owner)[position]; }
        Iterator & operator++() {
            ++position;
            return *this;
        }
        Iterator operator++(int) {
            auto previous = *this;
            ++*this;
            return previous;
        }
        bool operator==(const Iterator &) const = default;
    };

    struct DependencyIterator {
        using iterator_category = std::forward_iterator_tag;
        using value_type = TemplateDependency;
        using difference_type = std::ptrdiff_t;
        using reference = TemplateDependency;
        using pointer = void;

        const TemplateGraph * owner;
        std::size_t position;

        TemplateDependency operator*() const { return owner->dependency(position); }
        DependencyIterator & operator++() {
            ++position;
            return *this;
        }
        DependencyIterator operator++(int) {
            auto previous = *this;
            ++*this;
            return previous;
        }
        bool operator==(const DependencyIterator &) const = default;
    };

    class Dependencies {
    public:
        explicit Dependencies(const TemplateGraph & owner) : owner_(&owner) {}
        DependencyIterator begin() const { return { owner_, 0 }; }
        DependencyIterator end() const { return { owner_, size() }; }
        std::size_t size() const { return owner_->dependency_locations_.empty() ? owner_->local_dependencies_.size() : owner_->dependency_locations_.size(); }
        TemplateDependency operator[](std::size_t index) const { return owner_->dependency(index); }
        TemplateDependency at(std::size_t index) const { return owner_->dependency_at(index); }

    private:
        const TemplateGraph * owner_;
    };

    // Reads return values because shared local endpoints need source rebinding.
    Iterator begin() const { return { this, 0 }; }
    Iterator end() const { return { this, size() }; }
    std::size_t size() const { return locations_.empty() ? local_.size() : locations_.size(); }
    TemplateNode operator[](std::size_t index) const;
    TemplateNode at(std::size_t index) const;
    void push_back(TemplateNode node) { local_.push_back(node); }
    std::size_t boundary_count() const { return boundary_locations_.empty() ? local_boundaries_.size() : boundary_locations_.size(); }
    TemplateBoundary boundary(std::size_t index) const;
    void add_boundary(TemplateBoundary point) { local_boundaries_.push_back(point); }
    void add_dependency(TemplateDependency edge) { local_dependencies_.push_back(edge); }
    Dependencies dependencies() const { return Dependencies(*this); }
    std::size_t connection_count() const { return connection_locations_.empty() ? local_connections_.size() : connection_locations_.size(); }
    TemplateConnectionEndpoint connection(std::size_t index) const;
    void add_connection(TemplateConnectionContract contract, BoundaryId remote, SourceLane lane) { local_connections_.push_back({ contract, remote, lane }); }
    void bind_model_rule(std::size_t node, const ModelParameterRule & rule) { model_rules_.emplace(node, &rule); }
    const ModelParameterRule & model_rule(std::size_t node) const;
    void bind_collective_rule(std::size_t node, const CollectiveParameterRule & rule) { collective_rules_.emplace(node, &rule); }
    const CollectiveParameterRule & collective_rule(std::size_t node) const;

    // Producer finalization, once, after parameter binding. Regions are disjoint.
    // The source-owned pool outlives every definition and target instance.
    void share(const TemplateDefinition & source, std::span<const std::size_t> regions, TemplateRegionStructures & pool);
    std::size_t shared_regions() const { return bindings_.size(); }

private:
    struct Location {
        std::size_t region; // Zero denotes unshared records; bindings use one-based indices.
        std::size_t member; // Dependencies index internal edges, then dependency ports.
    };
    struct RemoteBoundaryBinding {
        BoundaryId source_boundary;
        std::optional<std::size_t> local_boundary; // Present when the endpoint belongs to this serving definition.
    };
    struct ObservedConnection {
        TemplateConnectionContract contract;
        BoundaryId remote_source_boundary;
        SourceLane remote_lane;
    };
    struct Binding {
        const TemplateRegionStructure * structure;
        std::vector<std::size_t> boundaries;
        std::vector<RemoteBoundaryBinding> remote_boundaries;
        std::vector<std::optional<std::size_t>> lanes; // Peer-only resources have no lane in this definition.
    };

    TemplateNode resolve(std::size_t index, const Location & location) const;
    TemplateDependency dependency(std::size_t index) const;
    TemplateDependency dependency_at(std::size_t index) const;
    TemplateDependency resolve_dependency(const Location & location) const;
    std::vector<TemplateBoundary> local_boundaries_;
    std::vector<Location> boundary_locations_;
    std::vector<TemplateNode> local_;
    std::vector<Location> locations_;
    std::vector<Binding> bindings_;
    std::vector<TemplateDependency> local_dependencies_;
    std::vector<Location> dependency_locations_;
    std::vector<ObservedConnection> local_connections_;
    std::vector<Location> connection_locations_;
    std::map<std::size_t, const ModelParameterRule *> model_rules_;
    std::map<std::size_t, const CollectiveParameterRule *> collective_rules_;
};

struct TemplateNodeObservation {
    SourceSpanId source_span;
    const SourceKernel * kernel; // Borrowed from TemplateSource::kernels.
    std::optional<std::size_t> source_scope;
    const SourceCommunicationTask * communication = nullptr; // Borrowed from this source definition's collective tasks.
    const SourceTransfer * transfer = nullptr;               // Borrowed from TemplateSource::transfers.
    std::optional<DeviceCommandWork> device_command = std::nullopt;
};

// Each insertion owns its target descriptor. Donor observations and definition
// remain immutable in the bank, including when the same span is reused twice.
// observation_slot stays donor-local while begin/end are relocated to the target.
struct TemplateInsertedNode {
    TemplateNode node;
    const TemplateDefinition * definition;
    std::optional<SourceKernel> kernel_override = std::nullopt;
    std::optional<SourceCommunicationTask> communication_override = std::nullopt;
    std::optional<TemplateStreamWait> stream_wait = std::nullopt;
    std::optional<SourceTransfer> transfer_override = std::nullopt;
    std::optional<std::size_t> request_slot = std::nullopt;          // Target request for relocated member work and parameters.
    std::optional<BatchFilterBinding> filter_binding = std::nullopt; // Target call role for a relocated filtering helper.

    const TemplateNodeObservation & observation() const;

    const SourceTransfer * transfer() const { return transfer_override ? &*transfer_override : observation().transfer; }
    const SourceKernel * kernel() const { return kernel_override ? &*kernel_override : observation().kernel; }
    const SourceCommunicationTask * communication() const { return communication_override ? &*communication_override : observation().communication; }
};

struct TemplateConnection {
    std::size_t source_edge;
    SourceRelation relation;
    TemplateConnectionDirection direction;
    std::size_t local_boundary;
    BoundaryId remote_source_boundary;
    std::vector<std::size_t> remote_source_calls; // Empty means the other endpoint has no observed owner.
    std::optional<TemplateResource> resource;
    std::optional<std::uint64_t> remote_order;
};

// Source provenance is separate from the reusable local connection contract.
struct TemplateConnectionObservation {
    std::size_t source_edge;
    std::vector<std::size_t> remote_source_calls;
    std::optional<TemplateResource> resource;
    std::optional<std::uint64_t> remote_order;
};

struct TemplateRegion {
    std::size_t source_call;
    std::size_t parent_call;
    std::optional<std::string> binding;
    std::optional<SourceNativeRegion> module;
    std::vector<std::size_t> nodes; // Indices in the enclosing definition; never additional executable nodes.
    std::vector<std::size_t> inputs;
    std::vector<std::size_t> outputs;
    std::optional<std::size_t> request_slot = std::nullopt; // Producer-resolved slot in source_requests, inherited by nested regions.
};

struct RotaryPositionDependency {
    std::size_t producer; // Node indices in the same definition, not new work.
    std::size_t consumer;
};

struct TemplateOperationWindow {
    SourceSpanId source_span;
    std::size_t begin;
    std::size_t end;
};

struct TemplateOperation {
    std::string name;
    TemplateOperationWindow host;
    std::vector<TemplateOperationWindow> workers;
    std::vector<SourceSpanId> device_spans;
    std::optional<std::string> input_shapes = std::nullopt;
    std::optional<std::string> input_types = std::nullopt;
    std::vector<SourceSpanId> collective_spans = {};
};

// A view into one definition. Boundary membership is distinct from node
// ownership because external CPU services can share an operation endpoint.
struct TemplateSubgraphRegion {
    std::vector<std::size_t> nodes;
    std::vector<std::size_t> boundaries;
    std::vector<std::size_t> crossing_nodes;
    // Outside work can share endpoints with the region; its extent must survive.
    std::vector<std::size_t> retained_dependencies;
    std::vector<std::size_t> internal_dependencies;
    std::vector<std::size_t> incoming_dependencies;
    std::vector<std::size_t> outgoing_dependencies;
    std::vector<std::size_t> external_connections;
    std::vector<std::size_t> incoming_rotary_dependencies;
    std::vector<std::size_t> outgoing_rotary_dependencies;

    nlohmann::json describe() const;
};

// A resource entry/exit for the currently qualified region replacement rules.
// Cross-rank notifications and arbitrary completion exports remain separate
// interfaces; this is not a complete shared model-region contract.
struct TemplateRegionPort {
    TemplateResource resource;
    std::size_t entry;
    std::size_t exit;
    std::optional<std::size_t> device_done = std::nullopt;

    nlohmann::json describe() const;
};

struct TemplateRegionResourceAnchor {
    TemplateRegionPort port;
    std::size_t region;
    bool predecessor;
};

// Ports of a qualified serial device operation; CPU windows stay on TemplateOperation.
struct TemplateDeviceOperationPorts {
    std::size_t stream_entry;
    std::size_t stream_exit;
    std::size_t device_done;

    nlohmann::json describe() const;
};

// Collective stream ordering fans out to all tasks. Cross-rank notifications
// are group-internal interfaces and must be rebound by the joint planner.
struct TemplateCollectiveOperationPorts {
    std::optional<std::size_t> stream_predecessor;
    std::size_t stream_completion;
    std::size_t enclosing_completion;
    std::vector<std::size_t> notification_connections;

    nlohmann::json describe() const;
};

struct TemplateOperationRewrite {
    std::optional<std::size_t> source_region; // Region identity for causal ordering of joint resource insertions.
    std::vector<std::size_t> nodes;
    std::vector<std::size_t> dependencies;
    std::vector<TemplateDependency> added_dependencies;
    std::vector<std::size_t> rotary_dependencies;
    std::optional<std::size_t> rotary_producer = std::nullopt;
    std::vector<TemplateInsertedNode> inserted_nodes;
    std::vector<TemplateInsertedBoundary> inserted_boundaries;
    std::vector<TemplateLane> inserted_lanes;
    std::vector<TemplateRegionPort> added_resource_ports;

    // External ports are explicitly bound; unmapped internal points are allocated
    // in this plan after the recipient's source boundaries. Lane mapping is explicit.
    // Kernel/communication observations remain owned by the bank, not this plan.
    std::map<std::size_t, std::size_t> insert_subgraph(const TemplateDefinition & donor, const TemplateDefinition & recipient,
                                                       std::span<const std::size_t> nodes, std::span<const std::size_t> dependencies,
                                                       std::map<std::size_t, std::size_t> boundary_mapping,
                                                       const std::map<std::size_t, std::size_t> & lane_mapping);
};

// Observation metadata references existing local nodes; it creates no extra
// communication service and does not imply a qualified target transformation.
struct TemplateCollective {
    SourceCollective observation;
    std::size_t envelope_node;
    std::vector<std::size_t> task_nodes;
    std::vector<std::size_t> entry_boundaries;
    std::vector<std::size_t> exit_boundaries;
    std::vector<std::size_t> internal_dependencies;
    std::optional<std::size_t> completion = std::nullopt;
};

// Preserves request multiplicity and a producer-qualified length branch.
struct RequestLengthDomain {
    std::uint32_t minimum_input_tokens;
    std::uint32_t maximum_input_tokens;
    std::uint32_t minimum_context_tokens;
    std::uint32_t maximum_context_tokens;

    bool accepts(const BatchInput & input) const {
        for (const auto & request : input.requests) {
            if (request.context_tokens < minimum_context_tokens || request.context_tokens > maximum_context_tokens
                || request.input_tokens < minimum_input_tokens || request.input_tokens > maximum_input_tokens)
                return false;
        }
        return true;
    }
};

enum class KvRegionKind { Allocation, ReleaseGroup };

struct KvRegionBranch {
    KvRegionKind kind;
    std::size_t region;
    bool changes_pages;
    std::size_t minimum_members = 0;
    std::size_t maximum_members = 0;

    bool accepts_members(std::size_t count) const { return count >= minimum_members && count <= maximum_members; }
};

struct TemplateDefinition {
    // Bound by the owning bank after loading; local IDs remain asset-local.
    std::filesystem::path source_manifest;

    TemplateDefinition(StepKind kind, BatchPhase source_phase, std::uint32_t source_rank, std::size_t call)
        : step(kind),
          phase(source_phase),
          rank(source_rank),
          source_call(call) {}
    void read_structure(const nlohmann::json & source, const nlohmann::json & graph, const nlohmann::json & regions, const nlohmann::json & calls,
                        const std::map<SourceSpanId, SourceKernel> & kernels, std::vector<TemplateConnection> source_connections);

    TemplateSubgraphRegion operation_region(const TemplateOperation & operation) const;
    TemplateSubgraphRegion workspace_region(const TemplateWorkspaceUse & use) const;
    TemplateSubgraphRegion semantic_region(const TemplateRegion & source) const;
    bool is_cpu_region(const TemplateRegion & region) const;
    bool is_cpu_region(std::span<const std::size_t> nodes) const;
    bool has_resource_roles() const;
    bool matches_running_filter(const BatchInput & target) const;
    std::expected<std::vector<TemplateRegionPort>, std::string> region_ports(const TemplateSubgraphRegion & region) const;
    bool region_precedes(const TemplateRegion & first, const TemplateRegion & second) const;
    std::expected<std::optional<TemplateRegionResourceAnchor>, std::string> region_resource_anchor(const TemplateRegion & region,
                                                                                                   const TemplateResource & resource) const;
    std::expected<TemplateDeviceOperationPorts, std::string> device_operation_ports(const TemplateOperation & operation,
                                                                                    const TemplateSubgraphRegion & region) const;
    std::expected<TemplateCollectiveOperationPorts, std::string> collective_operation_ports(const TemplateOperation & operation,
                                                                                            const TemplateSubgraphRegion & region) const;
    bool is_resource_wait_import(const TemplateConnection & connection) const;
    bool is_region_wait_import(const TemplateDependency & edge, const TemplateSubgraphRegion & region) const;
    bool matches_transition(const BatchInput & target) const;
    bool matches_partial_filter(BatchFilterBinding source_role, BatchFilterBinding target_role, const BatchInput & target) const;
    std::expected<TemplateOperationRewrite, std::string> plan_region_replacement(const TemplateRegion & region, const TemplateDefinition & donor,
                                                                                 const TemplateRegion & donor_region) const;
    std::expected<TemplateOperationRewrite, std::string> plan_workspace_replacement(const TemplateWorkspaceUse & use, const TemplateDefinition & donor,
                                                                                    const TemplateWorkspaceUse & donor_use) const;
    std::expected<TemplateOperationRewrite, std::string> plan_region_sequence(std::span<const std::size_t> source_members,
                                                                              std::span<const TemplateRegionReference> members) const;
    std::optional<std::size_t> logits_gather_reshape() const;
    std::optional<std::size_t> logits_view_service() const;
    std::expected<TemplateOperationRewrite, std::string> plan_position_materialization_removal(const TemplateOperation & operation) const;
    std::expected<TemplateOperationRewrite, std::string> plan_operation_replacement(const TemplateOperation & operation, const TemplateDefinition & donor,
                                                                                    const TemplateOperation & donor_operation) const;

    StepKind step;
    BatchPhase phase;
    std::uint32_t rank;
    std::size_t source_call;
    std::optional<std::uint64_t> source_batch;
    std::uint64_t source_order; // Native serving invocation order within its rank.
    std::optional<BatchTransitionState> transition;
    std::optional<std::size_t> transition_region;
    bool transition_latch_parameterized = false;      // Prefix work does not branch on the incoming admission latch.
    bool transition_merge_parameters = false;         // Qualified row rebinding within the same nonempty merge branch.
    bool transition_cpu_parameters = false;           // Non-indexing membership and scalar state changes within the same prefix shape.
    std::optional<std::size_t> running_update_region; // Qualified non-retracting update, including its membership filter.
    std::optional<AdmissionEntryState> admission;
    std::optional<AdmissionWork> admission_work;
    std::optional<std::size_t> admission_cpu_request_count; // Qualified CPU-only admission; zero denotes a pre-adder early return.
    std::vector<std::size_t> admission_regions;             // Ordered accepted candidates; first initializes state, later members insert.
    std::vector<BatchFilter> filters;
    std::map<BatchFilterBinding, std::size_t> filter_regions; // Producer-qualified calls to the same membership helper.
    std::optional<BatchMerge> merge;
    std::string binding; // Native binding that supplied this definition, retained across target instantiation.
    std::vector<std::size_t> source_subtree;

    std::size_t entry;
    std::map<PortKind, std::size_t> ports; // Indices in boundaries, as are node/dependency endpoints and inputs/outputs.
    std::optional<SourceTensorLayout> input_layout;
    std::vector<SourceRequest> source_requests;
    std::optional<RequestLengthDomain> length_domain;
    // One observation per request slot; unknown is distinct from unfinished.
    std::vector<std::optional<bool>> publication_finishes;
    std::optional<PeriodicOutputPolicy> output_policy;
    std::optional<PublicationParameterLayout> publication_layout;
    std::vector<std::size_t> publication_regions;            // Ordered, producer-qualified request publication bodies.
    std::vector<std::size_t> request_initialization_regions; // Ordered, interchangeable cold request initializers.
    bool publication_cpu_progress = false;                   // Unfinished, non-emitting Decode with preserved CPU work and observed waits.

    std::vector<TemplateLane> lanes;
    std::vector<TemplateBoundaryObservation> boundary_observations; // Same local indices; owned by this source definition.
    TemplateGraph graph;
    std::vector<TemplateNodeObservation> node_observations; // Source-owned bindings, indexed only by local observation_slot.

    const TemplateNodeObservation & observation(const TemplateNode & node) const { return node_observations[node.observation_slot]; }
    TemplateNodeObservation & observation(const TemplateNode & node) { return node_observations[node.observation_slot]; }

    std::vector<TemplateRegion> regions;
    std::optional<KvRegionBranch> kv_region;
    std::vector<TemplateOperation> operations;
    std::vector<TemplateWorkspaceUse> workspace_uses;
    std::map<SourceSpanId, BatchOperationVariant> batch_operations;
    std::map<SourceSpanId, TransferParameterLayout> transfer_layouts;
    std::optional<std::size_t> logits_reshape_operation; // Producer-qualified index in operations.
    std::vector<TemplateCollective> collectives;
    std::map<SourceSpanId, ModelParameterObservation> model_kernel_layouts;
    std::map<SourceSpanId, AttentionParameterObservation> attention_kernel_layouts;
    std::map<SourceSpanId, std::expected<KvParameterLayout, std::string>> kv_kernel_layouts;
    std::map<SourceSpanId, std::expected<RequestPoolParameterLayout, std::string>> request_pool_layouts;
    std::map<SourceSpanId, std::expected<BatchParameterLayout, std::string>> batch_kernel_layouts;
    std::vector<TemplateStreamWait> stream_waits;
    std::vector<RotaryPositionDependency> rotary_position_dependencies;
    std::vector<TemplateConnectionObservation> connection_observations;
    TemplateConnection connection(std::size_t index) const;
    std::vector<BoundaryId> inputs;
    std::vector<BoundaryId> outputs;

private:
    TemplateSubgraphRegion execution_region(std::span<const TemplateOperationWindow> windows, const std::set<SourceSpanId> & observed_members) const;
    std::expected<TemplateOperationRewrite, std::string> plan_subgraph_replacement(const TemplateSubgraphRegion & recipient_cut,
                                                                                   const TemplateDefinition & donor, const TemplateSubgraphRegion & donor_cut,
                                                                                   const TemplateRegion * region) const;
    TemplateSubgraphRegion complete_region(TemplateSubgraphRegion region) const;
    std::expected<TemplateSubgraphRegion, std::string> cpu_sequence_separator(std::size_t entry, std::size_t exit, std::size_t parent) const;
    std::optional<std::size_t> operation_view_service(const TemplateOperation & operation) const;
    void bind_collective_boundaries();
    std::expected<TemplateOperationRewrite, std::string> plan_device_operation_removal(const TemplateOperation & operation) const;
};

// Executable membership belongs to the instance; source identities and port
// boundaries remain stable when a finite rule removes work.
class TemplateTopology {
public:
    explicit TemplateTopology(const TemplateDefinition & definition);
    // Returned offsets relocate each plan's inserted endpoints into this instance.
    std::expected<std::vector<std::size_t>, std::string> apply_operations(const TemplateDefinition & definition,
                                                                          std::span<const TemplateOperationRewrite> removals);

    const std::vector<std::size_t> & nodes() const { return nodes_; }
    const std::vector<std::size_t> & dependencies() const { return dependencies_; }
    const std::vector<TemplateDependency> & added_dependencies() const { return added_dependencies_; }
    const std::vector<RotaryPositionDependency> & rotary_dependencies() const { return rotary_dependencies_; }
    const std::vector<TemplateInsertedNode> & inserted_nodes() const { return inserted_nodes_; }
    const std::vector<TemplateInsertedBoundary> & inserted_boundaries() const { return inserted_boundaries_; }
    const std::vector<TemplateLane> & inserted_lanes() const { return inserted_lanes_; }
    const std::vector<TemplateRegionPort> & added_resource_ports() const { return added_resource_ports_; }
    std::vector<std::vector<std::size_t>> successors(const TemplateDefinition & definition) const;
    void bind_inserted_kernel(std::size_t index, SourceKernel kernel);
    void bind_inserted_transfer(std::size_t index, SourceTransfer transfer);

private:
    std::vector<std::size_t> nodes_;
    std::vector<std::size_t> dependencies_;
    std::vector<TemplateDependency> added_dependencies_;
    std::vector<RotaryPositionDependency> rotary_dependencies_;
    std::vector<TemplateInsertedNode> inserted_nodes_;
    std::vector<TemplateInsertedBoundary> inserted_boundaries_;
    std::vector<TemplateLane> inserted_lanes_;
    std::vector<TemplateRegionPort> added_resource_ports_;
    std::size_t source_boundary_count_;
    std::size_t source_lane_count_;
};

struct TemplateEndpoint {
    WorkId work_id;
    std::uint32_t rank;
    std::size_t boundary;

    auto operator<=>(const TemplateEndpoint &) const = default;
};

// Observed shared points retain their source identity. Inserted points belong
// only to one target instance, even when they reuse the same donor evidence.
struct TemplateSourceIdentity {
    std::string_view manifest; // Borrowed from an immutable bank definition.
    std::size_t local;

    auto operator<=>(const TemplateSourceIdentity &) const = default;
};

using TemplateBoundaryIdentity = std::variant<TemplateSourceIdentity, TemplateEndpoint>;

enum class TemplateConnectionRule { Unresolved, NativeBoundary, CpuThreadProjection, ResourcePredecessor, InitialResource, ResourceExport };

struct TemplateConnectionInstance {
    TemplateConnectionRule rule = TemplateConnectionRule::Unresolved;
    std::vector<TemplateEndpoint> peers;
};

struct TemplateTargetConnection {
    TemplateConnection connection;
    TemplateConnectionInstance binding;
};

struct TemplateTargetStreamWait {
    TemplateStreamWait wait;
    TemplateConnectionInstance binding;
};

struct TemplateWorkspaceUseInstance {
    const TemplateDefinition * definition; // Source owner may differ from the enclosing instance after donor insertion.
    std::size_t source_use;
    std::size_t begin;
    std::size_t end;
    std::optional<WorkspaceDemand> demand;
};

struct TemplateInstance {
    TemplateInstance(WorkId work, const TemplateDefinition & source);

    WorkId work_id;
    const TemplateDefinition * definition; // Bank-owned immutable definition; the bank outlives all runtime work.
    TemplateTopology topology;
    // Target connections own their membership and local endpoints independently of the source.
    std::vector<TemplateTargetConnection> connections;
    std::map<SourceSpanId, SourceKernel> kernel_overrides;
    std::map<SourceSpanId, SourceTransfer> transfer_overrides;
    std::vector<TemplateWorkspaceUseInstance> workspace_uses; // Only uses present in the final target topology; no capacity mutation.
    std::optional<PublicationWork> publication_work;
    std::map<SourceSpanId, SourceCommunicationTask> communication_overrides;
    std::vector<TemplateTargetStreamWait> stream_waits;
    std::map<std::size_t, TemplateConnectionInstance> resource_imports;
    // Cross-instance ordering introduced by a qualified new resource interface.
    std::vector<std::pair<TemplateEndpoint, std::size_t>> resource_dependencies;

    void collect_stream_waits(); // Called after topology rewrites, before binding prior target submissions.
    std::expected<void, std::string> bind_inserted_waits();
    std::expected<void, std::string> bind_workspace_uses(const BatchInput & input);

    TemplateBoundary boundary(std::size_t index) const;
    const TemplateLane & lane(std::size_t index) const;
    TemplateBoundaryIdentity boundary_identity(std::size_t index) const;
    const SourceKernel * kernel(const TemplateNode & node) const;
    const SourceTransfer * transfer(const TemplateNode & node) const;
    const SourceCommunicationTask * communication(const TemplateNode & node) const;

private:
    std::expected<void, std::string> append_workspace_use(const TemplateDefinition & source, const TemplateNode & node, const BatchInput & input);
    static std::vector<bool> reachable_boundaries(const std::vector<std::vector<std::size_t>> & adjacency, std::vector<std::size_t> pending);
};

enum class TemplateGapKind { Observation, Rule };

struct TemplateGap {
    TemplateGapKind kind;
    std::string reason;
    StepKind step;
    BatchPhase phase;
    std::uint32_t rank;
};

// One assembly belongs to one candidate. It never shares frontiers with another run.
class TemplateAssembly {
public:
    std::expected<void, std::string> connect(std::span<TemplateInstance *> instances);

private:
    static void prune_frontier(std::map<TemplateBoundaryIdentity, TemplateEndpoint> & candidates,
                               const std::map<TemplateBoundaryIdentity, std::vector<TemplateBoundaryIdentity>> & adjacency);

    // An absent resource has no prior modeled work. An empty value is an observed
    // resource whose export is unknown, and must not be treated as initially idle.
    std::map<TemplateResource, std::vector<TemplateEndpoint>> frontiers_;
};

// Produced once and then owned by the consumer. Nodes borrow immutable kernel
// observations from this same asset; native parsers do not cross the boundary.
struct TemplateSource {
    TemplateSource() = default;
    TemplateSource(const TemplateSource &) = delete;
    TemplateSource & operator=(const TemplateSource &) = delete;
    TemplateSource(TemplateSource &&) = default;
    TemplateSource & operator=(TemplateSource &&) = default;

    std::filesystem::path manifest_path;
    std::string model_path;
    nlohmann::json configuration;
    std::uint32_t ranks;
    std::uint32_t page_size_tokens;
    Environment deployment;
    std::string resolution;
    std::optional<TemplateGap> source_gap;
    std::optional<std::string> initialization_gap;
    std::expected<AdmissionPolicy, TemplateGap> admission_policy =
        std::unexpected(TemplateGap{ TemplateGapKind::Observation, "admission_policy_not_observed", StepKind::Schedule, BatchPhase::Idle, 0 });
    std::map<SourceSpanId, SourceKernel> kernels;
    std::map<SourceSpanId, SourceTransfer> transfers;
    std::set<ModelParameterRule> model_rules;
    std::set<WorkspaceDemandLayout> workspace_rules;
    std::set<AttentionParameterRule> attention_rules;
    std::set<CollectiveParameterRule> collective_rules;
    TemplateRegionStructures region_structures;
    std::map<std::tuple<StepKind, BatchPhase, std::uint32_t>, std::vector<TemplateDefinition>> definitions;
    nlohmann::json gaps;
    std::vector<TemplateWorkspacePool> workspace_pools;
    std::optional<std::string> workspace_binding_gap;
    RequestPoolCapacityDomain request_pool_domain;
    RequestPoolCapacityDomain initialization_pool_domain;  // Producer-qualified reuse of the post-warmup capacities.
    std::optional<std::uint32_t> sequence_capacity_tokens; // Processed tokens, including the current query.
    std::vector<std::string> transforms;
    std::vector<std::string> limits;
};

// Source definitions are immutable. Selection preserves the observed rank/batch
// cohort; target descriptors and topology change only through qualified rules.
class TemplateBank {
public:
    explicit TemplateBank(TemplateSource source) : source_(std::move(source)) {
        for (auto & [key, definitions] : source_.definitions)
            for (auto & definition : definitions) definition.source_manifest = source_.manifest_path;
        bind_sources({});
    }

    // Borrowed sources are qualified by the caller; all owners remain alive during prediction.
    // Each batch still selects one complete native cohort before local rewrites.
    void bind_sources(std::span<const TemplateBank * const> banks, std::span<const TemplateDefinition * const> operation_sources = {});

    const TemplateSource & source() const { return source_; }

    // One nonempty controller batch; returned rows follow invocation order.
    std::expected<std::vector<std::vector<TemplateInstance>>, TemplateGap> instantiate(std::span<const InvokeStep> invocations,
                                                                                       std::uint32_t request_capacity) const;

    std::uint32_t page_size_tokens() const;
    const std::expected<AdmissionPolicy, TemplateGap> & admission_policy() const;
    std::span<const TemplateWorkspacePool> workspace_pools() const { return source_.workspace_pools; }
    const TemplateWorkspacePool & workspace_pool(const TemplateDefinition & definition, std::size_t index) const;
    std::expected<TemplateOperationRewrite, std::string> plan_workspace_replacement(const TemplateInstance & instance, const TemplateWorkspaceUseInstance & use,
                                                                                    WorkspaceBranch branch, const BatchInput & input) const;
    std::expected<void, TemplateGap> qualify(std::uint32_t request_capacity) const;
    std::expected<void, TemplateGap> qualify_initialization(std::uint32_t request_capacity) const;
    nlohmann::json describe() const;

private:
    std::expected<std::vector<TemplateOperationRewrite>, std::string> plan_materializations(const TemplateDefinition & source, const BatchInput & target) const;

    std::expected<void, std::string> parameterize_insertions(TemplateTopology & topology, const InvokeStep & invocation, std::uint32_t request_capacity) const;

    // Failure order follows qualification, never the spelling of a gap reason.
    enum class MatchStage { Source, RequestCount, Publication, Shape, Structure, Communication };
    struct SelectionFailure {
        MatchStage stage;
        TemplateGap gap;
    };

    std::expected<TemplateInstance, SelectionFailure> instantiate_step(const TemplateSource & anchor_source, const InvokeStep & invocation,
                                                                       std::optional<std::uint64_t> source_batch, std::uint32_t rank,
                                                                       std::uint32_t request_capacity) const;

    std::expected<void, TemplateGap> parameterize_communications(std::span<TemplateInstance> instances, const InvokeStep & invocation) const;

    std::expected<TemplateInstance, std::string> instantiate_source(const TemplateDefinition & source, const InvokeStep & invocation,
                                                                    std::uint32_t request_capacity) const;

    std::expected<std::optional<TemplateOperationRewrite>, std::string> plan_kv_region(const TemplateDefinition & source, const BatchInput & target) const;
    std::expected<std::vector<TemplateOperationRewrite>, std::string> plan_publication_members(const TemplateDefinition & source,
                                                                                               const BatchInput & target) const;
    std::expected<std::optional<TemplateOperationRewrite>, std::string> plan_transition(const TemplateDefinition & source, const BatchInput & target) const;
    std::expected<TemplateOperationRewrite, std::string> plan_filter(const TemplateDefinition & source, BatchFilterBinding role,
                                                                     const BatchInput & target) const;
    std::expected<std::optional<TemplateOperationRewrite>, std::string> plan_running_update(const TemplateDefinition & source, const BatchInput & target) const;
    std::expected<std::vector<TemplateOperationRewrite>, std::string> plan_batch_operations(const TemplateDefinition & source, const BatchInput & target) const;
    std::expected<std::optional<SourceKernel>, std::string> transform_kernel(const TemplateNodeObservation & observation, const TemplateDefinition & source,
                                                                             const InvokeStep & invocation, std::uint32_t request_capacity,
                                                                             std::optional<std::size_t> request_slot = std::nullopt,
                                                                             std::optional<BatchFilterBinding> filter_binding = std::nullopt) const;

    TemplateSource source_;
    std::vector<const TemplateDefinition *> operation_sources_; // Borrowed from workflow-owned banks; only declared independent operations.
    std::vector<const TemplateBank *> banks_;                   // Qualified local donors may lack an initialization contract.
    std::map<std::tuple<StepKind, BatchPhase, std::uint32_t>, std::vector<const TemplateDefinition *>> donors_;
};

} // namespace markov
