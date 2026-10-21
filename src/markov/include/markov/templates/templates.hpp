#pragma once

#include "markov/contracts/control.hpp"
#include "markov/contracts/environment.hpp"
#include "markov/contracts/service_work.hpp"
#include "markov/contracts/source_observations.hpp"
#include "markov/graph/dependency_paths.hpp"
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
#include <memory>
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
    std::size_t source_api;
    SourceSpanId marker;
    std::size_t begin; // Local allocator observation boundaries, inside its worker operation.
    std::size_t end;
    std::uint64_t requested_bytes;
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
    const CpuBoundary * cpu = nullptr;

    bool same_cpu_boundary(const TemplateInsertedBoundary & other) const { return cpu == other.cpu || (cpu && other.cpu && *cpu == *other.cpu); }
};

struct TemplateDefinition;
struct TemplateRankBinding;

struct TemplateRegionReference {
    const TemplateDefinition * definition;
    std::size_t sequence;
    std::size_t member;
};

enum class TemplateSequenceParameter { Request, IncomingRequest, ModelLayer, InputReception };

struct TemplateSubgraphRegion;
struct TemplateSequenceInput;
struct TemplateRegionSequence {
    std::vector<std::size_t> source_members;
    std::vector<std::size_t> members; // Fixed model expansion; request membership is supplied by the controller.
    TemplateSequenceParameter parameter = TemplateSequenceParameter::Request;
    std::vector<TemplateSubgraphRegion> separators;
    std::vector<TemplateSequenceInput> inputs;
    std::optional<std::string> gap;
    std::optional<std::size_t> repeated_separator;
};

struct TemplateSequenceRewrite {
    const TemplateRegionSequence * source; // Borrowed from the immutable source definition.
    std::vector<TemplateRegionReference> members;
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

// Actual peers belong to one source invocation, not its shared self structure.
struct TemplateConnectionEndpoint {
    TemplateConnectionContract contract;
    BoundaryId remote_source_boundary;
};

struct TemplateResourceSlot {
    SourceDomain domain;
    std::size_t rank; // Relative rank; zero is this serving definition's rank.
    std::optional<TemplateResourceRole> role = std::nullopt;
    auto operator<=>(const TemplateResourceSlot &) const = default;
};

// Local resource slots and qualified model rules belong to the shared graph.
// Actual lanes and cross-region peers remain source bindings. Shared self work
// does not establish interchangeable interfaces or qualify a target rewrite.
struct TemplateRegionStructure {
    std::vector<TemplateResourceSlot> resources;
    std::vector<TemplateBoundary> boundaries; // Lane indexes a local resource slot.
    std::vector<TemplateNode> nodes;
    std::vector<TemplateDependency> dependencies;
    std::map<std::size_t, const ModelParameterRule *> model_rules; // Source-owned canonical rules, indexed by local node.
    std::string semantic;
    std::strong_ordering operator<=>(const TemplateRegionStructure & other) const;
};

using TemplateRegionStructures = std::set<TemplateRegionStructure>;

// Iteration addresses source node slots; dependencies address source edge slots.
// Node and internal-dependency views restore endpoints through region bindings.
// Cross-region edges and external connections retain source-local endpoints once.
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
    std::size_t connection_count() const { return connections_.size(); }
    TemplateConnectionEndpoint connection(std::size_t index) const;
    void add_connection(TemplateConnectionContract contract, BoundaryId remote) { connections_.push_back({ contract, remote }); }
    void bind_model_rule(std::size_t node, const ModelParameterRule & rule) { model_rules_.emplace(node, &rule); }
    const ModelParameterRule * model_rule(std::size_t node) const;

    // Producer finalization, once, after parameter binding. Regions share only
    // their own work; TemplateDefinition::regions retains the invocation tree.
    // The source-owned pool outlives every definition and target instance.
    void share(const TemplateDefinition & source, std::span<const std::size_t> regions, TemplateRegionStructures & pool);
    std::size_t shared_regions() const { return bindings_.size(); }
    const TemplateRegionStructure * structure(std::size_t node) const;
    std::optional<std::size_t> local_boundary(std::size_t node, std::size_t boundary) const;

private:
    struct Location {
        std::size_t region; // Zero denotes unshared records; bindings use one-based indices.
        std::size_t member;
    };
    struct Binding {
        const TemplateRegionStructure * structure;
        std::vector<std::size_t> boundaries;
        std::vector<std::size_t> lanes;
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
    std::vector<TemplateConnectionEndpoint> connections_;
    std::map<std::size_t, const ModelParameterRule *> model_rules_;
};

struct TemplateNodeObservation {
    SourceSpanId source_span;
    const SourceKernel * kernel; // Borrowed from TemplateSource::kernels.
    std::optional<std::size_t> source_scope;
    const SourceTransfer * transfer = nullptr; // Borrowed from TemplateSource::transfers.
    std::optional<DeviceCommandWork> device_command = std::nullopt;
};

using TemplateSemanticPath = std::shared_ptr<const std::vector<std::string_view>>;

struct TemplateSemanticScope {
    std::vector<std::string_view> path;
    std::optional<std::size_t> request_slot;
    std::optional<std::uint32_t> model_layer;
    std::optional<std::size_t> input_slot;
};

// Cut membership proves ancestry. Rebinding replaces only that cut's prefix;
// observed semantic names below it remain shared by the relocated work.
TemplateSemanticPath rebase_semantic_path(const TemplateSemanticPath & path, std::size_t prefix_size, std::span<const std::string_view> target_prefix);

// Each insertion owns its target descriptor. Donor observations and definition
// remain immutable in the bank, including when the same span is reused twice.
// observation_slot stays donor-local while begin/end are relocated to the target.
struct TemplateInsertedNode {
    TemplateNode node;
    const TemplateDefinition * definition;
    std::optional<SourceKernel> kernel_override = std::nullopt;
    std::optional<TemplateStreamWait> stream_wait = std::nullopt;
    std::optional<SourceTransfer> transfer_override = std::nullopt;
    std::optional<std::size_t> request_slot = std::nullopt;          // Target request for relocated member work and parameters.
    std::optional<BatchFilterBinding> filter_binding = std::nullopt; // Target call role for a relocated filtering helper.
    std::optional<std::uint32_t> model_layer = std::nullopt;
    // The source recipient's semantic owner; donor provenance does not confer ownership.
    std::optional<std::size_t> recipient_region = std::nullopt;
    TemplateSemanticPath semantic_path = {};              // Complete target path, shared per observed scope/cut; null remains unknown.
    std::optional<std::size_t> input_slot = std::nullopt; // Incoming cohort member, independent of the selected model batch.

    const TemplateNodeObservation & observation() const;

    const SourceTransfer * transfer() const { return transfer_override ? &*transfer_override : observation().transfer; }
    const SourceKernel * kernel() const { return kernel_override ? &*kernel_override : observation().kernel; }
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

struct TemplateRegionScope {
    std::size_t begin;
    std::size_t end;
};

struct TemplateRegionPort {
    TemplateResource resource;
    std::size_t entry;
    std::size_t exit;
    std::optional<std::size_t> device_done = std::nullopt;
};

// A local cut owns work and internal edges, while retaining each crossing
// interface explicitly. Sharing endpoints never transfers an outside service.
struct TemplateSubgraphRegion {
    std::vector<std::size_t> nodes;
    std::vector<std::size_t> boundaries;
    std::vector<std::size_t> crossing_nodes;
    std::vector<std::size_t> retained_dependencies;
    std::vector<std::size_t> internal_dependencies;
    std::vector<std::size_t> incoming_dependencies;
    std::vector<std::size_t> outgoing_dependencies;
    std::vector<std::size_t> external_connections;
    std::vector<std::size_t> incoming_rotary_dependencies;
    std::vector<std::size_t> outgoing_rotary_dependencies;
    std::expected<std::vector<TemplateRegionPort>, std::string> resource_ports = std::unexpected("region_cut_not_declared");
    std::optional<std::string> replacement_gap; // Finite replacement may require a narrower interface than repetition.
};

enum class TemplateDependencyKind { Local, ModelData };

struct TemplateDependencyOrigin {
    TemplateDependencyKind kind;
    std::size_t slot;
    auto operator<=>(const TemplateDependencyOrigin &) const = default;
};

struct TemplateSequenceInput {
    std::size_t step; // Alternating source member/separator slots.
    TemplateDependencyOrigin dependency;
    // A resource tail advances with repetition. An interior export instead
    // belongs to one unique target producer; an external input has neither.
    std::optional<TemplateResource> previous_resource;
    std::optional<std::size_t> producer_step;
};

// Production completes this one cut after all derived dependencies and resource
// roles are bound. The invocation tree and the cut share one membership array.
struct TemplateRegion : TemplateSubgraphRegion {
    TemplateRegion(std::size_t call, std::size_t parent) : source_call(call), parent_call(parent) {}

    std::size_t source_call;
    std::size_t parent_call;
    std::string semantic;
    std::vector<std::size_t> inputs;
    std::vector<std::size_t> outputs;
    std::optional<std::size_t> request_slot = std::nullopt;      // Producer-resolved slot in source_requests, inherited by nested regions.
    std::optional<TemplateRegionScope> cpu_scope = std::nullopt; // Observed profiler region, including wrapper bookkeeping; not a CPU service duration.
    std::optional<std::size_t> input_slot = std::nullopt;
};

// The producer binds a native semantic call at its qualified transport cut.
// CPU preparation and local layout remain ordinary observed work.
struct TemplateCollectiveInterface {
    std::string position;
    CommunicationOperation operation;
    std::optional<CollectiveCountRule> count;
    std::string data_type;
    std::uint32_t group_size;
    TemplateRegionScope host; // Observation/context cuts, never a generated CPU service.
    std::size_t owner_region;
    std::size_t lane;
    std::vector<std::size_t> inputs;
    std::vector<std::size_t> outputs;
    std::optional<std::size_t> model_region;
    std::optional<std::uint32_t> model_layer;
    std::vector<std::size_t> substitute_nodes = {};     // Only transport and explicitly observed auxiliary device services.
    std::vector<std::size_t> transport_boundaries = {}; // Actual transport points, including pure internal task joins.
    std::vector<std::size_t> launch_inputs = {};        // Native CPU submission cuts; independent of device data readiness.
    std::vector<std::size_t> seam_dependencies = {};    // Observed stream edges split when PG1 submits no physical transport task.
    std::optional<HostMessageRule> host_message = std::nullopt;
};

struct TemplateCollectiveInstance {
    const TemplateDefinition * definition;
    std::size_t interface;
    TemplateRegionScope host;
    std::size_t ready;
    std::size_t done;
    std::optional<std::uint32_t> model_layer;
    const CollectiveWork * work = nullptr; // Borrowed from this batch's owning logical group.
    std::optional<std::size_t> recipient_region = std::nullopt;
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
};

struct TemplateRegionResourceAnchor {
    TemplateRegionPort port;
    std::size_t region;
    bool predecessor;
};

struct TemplateOwnedDependency : TemplateDependency {
    TemplateOwnedDependency(TemplateDependency edge, std::optional<std::size_t> owner = std::nullopt,
                            std::optional<TemplateDependencyOrigin> original = std::nullopt)
        : TemplateDependency(edge),
          recipient_region(owner),
          origin(original) {}
    TemplateOwnedDependency(SourceRelation relation, std::size_t source, std::size_t target, std::optional<std::size_t> owner = std::nullopt)
        : TemplateOwnedDependency(TemplateDependency{ relation, source, target }, owner) {}
    std::optional<std::size_t> recipient_region;
    std::optional<TemplateDependencyOrigin> origin; // Recipient edge identity survives endpoint relocation; copied donor edges have none.
};

struct TemplateOwnedResourcePort : TemplateRegionPort {
    TemplateOwnedResourcePort(TemplateRegionPort port, std::optional<std::size_t> owner) : TemplateRegionPort(port), recipient_region(owner) {}
    std::optional<std::size_t> recipient_region;
};

// Ports of a qualified serial device operation; CPU windows stay on TemplateOperation.
struct TemplateDeviceOperationPorts {
    std::size_t stream_entry;
    std::size_t stream_exit;
    std::size_t device_done;
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
    // Reused interfaces keep their target identity and take the donor's actual
    // observation. Multiple proposals remain visible until joint validation.
    std::vector<std::pair<std::size_t, TemplateInsertedBoundary>> rebound_boundaries;
    std::vector<TemplateLane> inserted_lanes;
    std::vector<TemplateRegionPort> added_resource_ports;
    std::optional<TemplateSequenceRewrite> sequence;

    // External ports are explicitly bound; unmapped internal points are allocated
    // in this plan after the recipient's source boundaries. Lane mapping is explicit.
    // Kernel/transfer observations remain owned by the bank, not this plan.
    std::map<std::size_t, std::size_t> insert_subgraph(const TemplateDefinition & donor, const TemplateDefinition & recipient,
                                                       std::span<const std::size_t> nodes, std::span<const std::size_t> dependencies,
                                                       std::map<std::size_t, std::size_t> boundary_mapping,
                                                       const std::map<std::size_t, std::size_t> & lane_mapping,
                                                       std::optional<std::pair<std::size_t, std::size_t>> scope_roots = std::nullopt);
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

    TemplateSubgraphRegion operation_region(const TemplateOperation & operation) const;
    TemplateSubgraphRegion workspace_region(const TemplateWorkspaceUse & use) const;
    void finalize_regions();
    void qualify_sequences();
    std::size_t declare_sequence(std::vector<std::size_t> members, TemplateSequenceParameter parameter = TemplateSequenceParameter::Request,
                                 std::vector<std::size_t> expanded = {});
    const TemplateSubgraphRegion & sequence_step(const TemplateRegionSequence & sequence, std::size_t step) const;
    bool contains_region(std::size_t ancestor, std::size_t region) const;
    std::optional<std::size_t> region_owner(std::span<const std::size_t> nodes) const;
    std::optional<TemplateSemanticScope> semantic_scope(std::optional<std::size_t> call, std::optional<std::size_t> root = std::nullopt) const;
    bool is_cpu_region(const TemplateRegion & region) const;
    bool is_cpu_region(std::span<const std::size_t> nodes) const;
    bool has_resource_roles() const;
    bool matches_running_filter(const BatchInput & target) const;
    std::expected<std::vector<TemplateRegionPort>, std::string> region_ports(const TemplateSubgraphRegion & region) const;
    std::partial_ordering region_order(const TemplateRegion & first, const TemplateRegion & second) const;
    std::expected<std::optional<TemplateRegionResourceAnchor>, std::string> region_resource_anchor(const TemplateRegion & region,
                                                                                                   const TemplateResource & resource) const;
    std::expected<TemplateDeviceOperationPorts, std::string> device_operation_ports(const TemplateOperation & operation,
                                                                                    const TemplateSubgraphRegion & region) const;
    bool is_resource_wait_import(const TemplateConnection & connection) const;
    bool is_region_wait_import(const TemplateDependency & edge, const TemplateSubgraphRegion & region) const;
    bool matches_transition(const BatchInput & target) const;
    bool matches_partial_filter(BatchFilterBinding source_role, BatchFilterBinding target_role, const BatchInput & target) const;
    std::expected<TemplateOperationRewrite, std::string> plan_region_replacement(const TemplateRegion & region, const TemplateDefinition & donor,
                                                                                 const TemplateRegion & donor_region) const;
    std::expected<TemplateOperationRewrite, std::string> plan_workspace_replacement(const TemplateWorkspaceUse & use, const TemplateDefinition & donor,
                                                                                    const TemplateWorkspaceUse & donor_use) const;
    std::expected<TemplateOperationRewrite, std::string> plan_region_sequence(std::size_t sequence, std::span<const TemplateRegionReference> members) const;
    std::optional<std::size_t> operation_view_service(const TemplateOperation & operation) const;
    std::expected<TemplateOperationRewrite, std::string> plan_view_materialization_removal(const TemplateOperation & operation,
                                                                                           std::optional<std::size_t> view_producer) const;
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
    std::optional<std::size_t> admission_sequence;          // Ordered accepted candidates; first initializes state, later members insert.
    std::vector<BatchFilter> filters;
    std::map<BatchFilterBinding, std::size_t> filter_regions; // Producer-qualified calls to the same membership helper.
    std::optional<BatchMerge> merge;

    std::size_t entry;
    std::map<PortKind, std::size_t> ports; // Local boundaries, shared with node and dependency endpoints.
    std::vector<SourceRequest> source_requests;
    std::vector<SourceIncomingRequest> source_incoming_requests;
    std::expected<bool, std::string> input_reception = std::unexpected("native_input_reception_not_observed"); // False means a known unsupported input path.
    std::vector<std::pair<std::size_t, std::size_t>> incoming_request_ready;                                   // Incoming slot -> actual native handler return.
    std::optional<std::size_t> incoming_request_sequence;
    std::optional<std::size_t> input_reception_sequence; // Native input prefix, independent of the later batch selection.
    std::optional<RequestLengthDomain> length_domain;
    // One observation per request slot; unknown is distinct from unfinished.
    std::vector<std::optional<bool>> publication_finishes;
    std::optional<PeriodicOutputPolicy> output_policy;
    std::optional<std::size_t> output_boundary; // Qualified token handoff, distinct from the control return.
    std::optional<PublicationParameterLayout> publication_layout;
    std::optional<std::size_t> publication_sequence;
    std::optional<std::size_t> request_initialization_sequence;
    bool publication_cpu_progress = false; // Unfinished, non-emitting Decode with preserved CPU work and observed waits.

    std::vector<TemplateLane> lanes;
    std::vector<TemplateBoundaryObservation> boundary_observations; // Same local indices; owned by this source definition.
    std::map<std::size_t, CpuBoundary> cpu_boundaries;
    TemplateGraph graph;
    std::vector<TemplateNodeObservation> node_observations; // Source-owned bindings, indexed only by local observation_slot.

    const TemplateNodeObservation & observation(const TemplateNode & node) const { return node_observations[node.observation_slot]; }
    TemplateNodeObservation & observation(const TemplateNode & node) { return node_observations[node.observation_slot]; }
    const CpuBoundary * cpu_boundary(std::size_t boundary) const {
        const auto found = cpu_boundaries.find(boundary);
        return found == cpu_boundaries.end() ? nullptr : &found->second;
    }

    std::vector<TemplateRegion> regions;
    std::vector<TemplateRegionSequence> sequences;
    std::optional<std::size_t> model_sequence;
    std::optional<KvRegionBranch> kv_region;
    std::vector<TemplateOperation> operations;
    std::vector<TemplateWorkspaceUse> workspace_uses;
    std::map<SourceSpanId, BatchOperationVariant> batch_operations;
    std::map<SourceSpanId, TransferParameterLayout> transfer_layouts;
    std::vector<TemplateCollectiveInterface> collective_interfaces;
    std::map<SourceSpanId, const AttentionParameterRule *> attention_kernel_layouts;
    std::map<SourceSpanId, std::expected<KvParameterLayout, std::string>> kv_kernel_layouts;
    std::map<SourceSpanId, std::expected<RequestPoolParameterLayout, std::string>> request_pool_layouts;
    std::map<SourceSpanId, std::expected<BatchParameterLayout, std::string>> batch_kernel_layouts;
    std::vector<TemplateStreamWait> stream_waits;
    std::vector<RotaryPositionDependency> rotary_position_dependencies;
    // Operation index -> view producer when one request makes the copy unnecessary.
    // A missing producer retains an observed copy whose replacement is unqualified.
    std::map<std::size_t, std::optional<std::size_t>> single_request_views;
    std::vector<TemplateConnectionObservation> connection_observations;
    TemplateConnection connection(std::size_t index) const;

private:
    TemplateSubgraphRegion execution_region(std::span<const TemplateOperationWindow> windows, const std::set<SourceSpanId> & observed_members) const;
    std::expected<TemplateOperationRewrite, std::string> plan_subgraph_replacement(const TemplateSubgraphRegion & recipient_cut,
                                                                                   const TemplateDefinition & donor, const TemplateSubgraphRegion & donor_cut,
                                                                                   const TemplateRegion * region,
                                                                                   std::optional<std::size_t> donor_root = std::nullopt) const;
    TemplateSubgraphRegion complete_region(TemplateSubgraphRegion region, std::optional<TemplateRegionScope> control = std::nullopt) const;
    TemplateSubgraphRegion sequence_separator(const TemplateRegionSequence & sequence, std::size_t previous, std::size_t next) const;
    void bind_sequence(TemplateRegionSequence & sequence) const;
    std::expected<std::vector<TemplateRegionPort>, std::string> bind_region_ports(TemplateSubgraphRegion & region,
                                                                                  std::optional<TemplateRegionScope> control) const;
};

// Executable membership belongs to the instance; source identities and port
// boundaries remain stable when a finite rule removes work.
class TemplateTopology {
public:
    explicit TemplateTopology(const TemplateDefinition & definition);
    std::expected<void, std::string> apply_operations(const TemplateDefinition & definition, std::span<const TemplateOperationRewrite> removals);
    std::expected<void, std::string> replace_sequence(const TemplateDefinition & definition, const TemplateSequenceRewrite & sequence,
                                                      const TemplateDefinition & donor, const TemplateTopology & prepared);

    const std::vector<std::size_t> & nodes() const { return nodes_; }
    const std::vector<std::size_t> & dependencies() const { return dependencies_; }
    const std::vector<TemplateOwnedDependency> & added_dependencies() const { return added_dependencies_; }
    const std::vector<RotaryPositionDependency> & rotary_dependencies() const { return rotary_dependencies_; }
    const std::vector<TemplateInsertedNode> & inserted_nodes() const { return inserted_nodes_; }
    const std::vector<TemplateInsertedBoundary> & inserted_boundaries() const { return inserted_boundaries_; }
    const std::map<std::size_t, TemplateInsertedBoundary> & rebound_boundaries() const { return rebound_boundaries_; }
    TemplateInsertedBoundary boundary_observation(const TemplateDefinition & definition, std::size_t boundary) const;
    const std::vector<TemplateLane> & inserted_lanes() const { return inserted_lanes_; }
    const std::vector<TemplateOwnedResourcePort> & added_resource_ports() const { return added_resource_ports_; }
    const std::vector<TemplateCollectiveInstance> & collective_interfaces() const { return collective_interfaces_; }
    const std::vector<std::pair<std::size_t, std::size_t>> & incoming_request_ready() const { return incoming_request_ready_; }
    std::size_t boundary_alias(std::size_t boundary, bool incoming = false) const;
    std::vector<std::vector<std::size_t>> successors(const TemplateDefinition & definition) const;
    void bind_inserted_kernel(std::size_t index, SourceKernel kernel);
    void bind_inserted_transfer(std::size_t index, SourceTransfer transfer);
    void bind_collective_work(std::size_t index, const CollectiveWork & work);

private:
    class SequenceExpansion;
    std::expected<void, std::string> apply_sequence(const TemplateDefinition & definition, const TemplateSequenceRewrite & sequence);
    std::vector<std::size_t> nodes_;
    std::vector<std::size_t> dependencies_;
    std::vector<TemplateOwnedDependency> added_dependencies_;
    std::vector<RotaryPositionDependency> rotary_dependencies_;
    std::vector<TemplateInsertedNode> inserted_nodes_;
    std::vector<TemplateInsertedBoundary> inserted_boundaries_;
    std::map<std::size_t, TemplateInsertedBoundary> rebound_boundaries_;
    std::vector<TemplateLane> inserted_lanes_;
    std::vector<TemplateOwnedResourcePort> added_resource_ports_;
    std::vector<TemplateCollectiveInstance> collective_interfaces_;
    std::vector<std::pair<std::size_t, std::size_t>> incoming_request_ready_;
    std::map<std::size_t, std::pair<std::size_t, std::size_t>> boundary_aliases_;
    std::size_t source_boundary_count_;
    std::size_t source_lane_count_;
};

struct TemplateEndpoint {
    WorkId work_id;
    std::uint32_t rank;
    std::size_t boundary;

    auto operator<=>(const TemplateEndpoint &) const = default;
};

struct TemplateCollectiveGroup {
    CollectiveWork work;
    std::vector<TemplateEndpoint> ready;
    std::vector<TemplateEndpoint> done;
    std::vector<std::pair<TemplateEndpoint, TemplateEndpoint>> completion_entry_dependencies = {};
};

// Observed shared points retain their source identity. Inserted points belong
// only to one target instance, even when they reuse the same donor evidence.
struct TemplateSourceIdentity {
    std::string_view manifest; // Borrowed from an immutable bank definition.
    std::size_t local;
    std::uint32_t replica;

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
    nlohmann::json effect = nullptr; // Diagnostics of this accepted use; capacities remain owned by WorkspaceState.
};

struct TemplateInstance {
    TemplateInstance(WorkId work, const TemplateDefinition & source, std::uint32_t target_rank, const TemplateRankBinding & rank_binding);

    WorkId work_id;
    std::uint32_t rank;
    const TemplateRankBinding * rank_binding; // Bank-owned control participation and output role, separate from local dependency gates.
    const TemplateDefinition * definition;    // Bank-owned immutable definition; the bank outlives all runtime work.
    TemplateTopology topology;
    // Target connections own their membership and local endpoints independently of the source.
    std::vector<TemplateTargetConnection> connections;
    std::map<SourceSpanId, SourceKernel> kernel_overrides;
    std::map<SourceSpanId, SourceTransfer> transfer_overrides;
    std::vector<TemplateWorkspaceUseInstance> workspace_uses; // Only uses present in the final target topology; no capacity mutation.
    std::vector<TemplateCollectiveGroup> collective_groups;   // One owner per logical operation; all participants retain local interfaces.
    std::vector<TemplateTargetStreamWait> stream_waits;
    std::map<std::size_t, TemplateConnectionInstance> resource_imports;
    // Cross-instance ordering introduced by a qualified new resource interface.
    std::vector<std::pair<TemplateEndpoint, std::size_t>> resource_dependencies;

    void collect_stream_waits(); // Called after topology rewrites, before binding prior target submissions.
    std::expected<void, std::string> bind_inserted_waits();
    std::expected<void, std::string> bind_workspace_uses(const BatchInput & input);

    TemplateBoundary boundary(std::size_t index) const;
    TemplateLane lane(std::size_t index) const;
    TemplateResource resource(TemplateResource source) const;
    TemplateSourceIdentity source_identity(std::size_t local) const;
    TemplateBoundaryIdentity boundary_identity(std::size_t index) const;
    const SourceKernel * kernel(const TemplateNode & node) const;
    const SourceTransfer * transfer(const TemplateNode & node) const;

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
    std::vector<TemplateEndpoint> frontier() const;

private:
    static void prune_frontier(std::map<TemplateBoundaryIdentity, TemplateEndpoint> & candidates, const DependencyPaths<TemplateBoundaryIdentity> & paths);

    // An absent resource has no prior modeled work. An empty value is an observed
    // resource whose export is unknown, and must not be treated as initially idle.
    std::map<TemplateResource, std::vector<TemplateEndpoint>> frontiers_;
};

struct TemplateRankBinding {
    std::uint32_t source_rank;
    std::vector<PortKind> completion_ports;
    bool publishes_outputs;
    bool receives_inputs;

    auto operator<=>(const TemplateRankBinding &) const = default;
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
    nlohmann::json model_configuration;
    nlohmann::json configuration;
    std::uint32_t source_rank;
    std::vector<TemplateRankBinding> rank_bindings;
    std::uint32_t page_size_tokens;
    Environment deployment;
    std::string resolution;
    std::optional<TemplateGap> source_gap;
    std::optional<std::string> steady_state_gap;
    std::expected<AdmissionPolicy, TemplateGap> admission_policy =
        std::unexpected(TemplateGap{ TemplateGapKind::Observation, "admission_policy_not_observed", StepKind::Schedule, BatchPhase::Idle, 0 });
    std::map<SourceSpanId, SourceKernel> kernels;
    std::map<SourceSpanId, SourceTransfer> transfers;
    std::set<ModelParameterRule> model_rules;
    std::set<WorkspaceDemandLayout> workspace_rules;
    std::set<AttentionParameterRule> attention_rules;
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

    std::uint32_t rank_count() const { return static_cast<std::uint32_t>(rank_bindings.size()); }
};

// Each local role keeps its observed native batch cohort. Independent role
// sessions meet through target work and interfaces, never source batch IDs.
class TemplateBank {
public:
    explicit TemplateBank(TemplateSource source);

    // Borrowed sources are qualified by the caller; all owners remain alive during prediction.
    void bind_sources(std::span<const TemplateBank * const> banks, std::span<const TemplateDefinition * const> operation_sources = {},
                      std::optional<std::uint32_t> request_capacity = std::nullopt);

    bool shares_source_contract(const TemplateBank & bank) const;

    const TemplateSource & source() const { return source_; }

    // One nonempty controller batch; returned rows follow invocation order.
    std::expected<std::vector<std::vector<TemplateInstance>>, TemplateGap> instantiate(std::span<const InvokeStep> invocations,
                                                                                       std::uint32_t request_capacity) const;

    std::uint32_t page_size_tokens() const;
    const std::expected<AdmissionPolicy, TemplateGap> & admission_policy() const;
    std::span<const TemplateWorkspacePool> workspace_pools() const { return workspace_pools_; }
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
    enum class MatchStage { Source, Input, RequestCount, Publication, Shape, Structure, Communication };
    struct SelectionFailure {
        MatchStage stage;
        TemplateGap gap;

        SelectionFailure(MatchStage stage, TemplateGap gap) : stage(stage), gap(std::move(gap)) {}
        SelectionFailure(MatchStage stage, TemplateGapKind kind, std::string reason, const InvokeStep & invocation, std::uint32_t rank)
            : stage(stage),
              gap{ kind, std::move(reason), invocation.step, invocation.input->phase, rank } {}
    };

    static std::expected<void, SelectionFailure> qualify_source(const TemplateDefinition & source, const InvokeStep & invocation);

    std::expected<TemplateInstance, SelectionFailure> instantiate_step(const TemplateSource & anchor_source, const InvokeStep & invocation,
                                                                       std::optional<std::uint64_t> source_batch, std::uint32_t rank,
                                                                       std::uint32_t request_capacity) const;

    std::expected<void, TemplateGap> parameterize_communications(std::span<TemplateInstance> instances, const InvokeStep & invocation) const;
    std::expected<void, std::string> bind_input_reception(TemplateTopology & topology, const TemplateDefinition & source, const BatchInput & target) const;
    static std::expected<std::optional<TemplateOperationRewrite>, std::string> plan_incoming_requests(const TemplateDefinition & source,
                                                                                                      const BatchInput & target);

    std::expected<TemplateInstance, std::string> instantiate_source(const TemplateDefinition & source, const InvokeStep & invocation,
                                                                    std::uint32_t request_capacity, std::uint32_t target_rank,
                                                                    const TemplateRankBinding & rank_binding) const;

    std::expected<std::optional<TemplateOperationRewrite>, std::string> plan_kv_region(const TemplateDefinition & source, const BatchInput & target) const;
    std::expected<std::vector<TemplateOperationRewrite>, std::string> plan_publication_members(const TemplateDefinition & source,
                                                                                               const BatchInput & target) const;
    std::expected<std::optional<TemplateOperationRewrite>, std::string> plan_transition(const TemplateDefinition & source, const BatchInput & target) const;
    std::expected<TemplateOperationRewrite, std::string> plan_filter(const TemplateDefinition & source, BatchFilterBinding role,
                                                                     const BatchInput & target) const;
    std::expected<std::optional<TemplateOperationRewrite>, std::string> plan_running_update(const TemplateDefinition & source, const BatchInput & target) const;
    std::expected<std::vector<TemplateOperationRewrite>, std::string> plan_batch_operations(const TemplateDefinition & source, const BatchInput & target) const;
    std::expected<std::optional<SourceKernel>, std::string> transform_kernel(const TemplateNode & node, const TemplateDefinition & source,
                                                                             const InvokeStep & invocation, std::uint32_t request_capacity,
                                                                             std::optional<std::size_t> request_slot = std::nullopt,
                                                                             std::optional<BatchFilterBinding> filter_binding = std::nullopt) const;

    TemplateSource source_;
    std::vector<TemplateWorkspacePool> workspace_pools_;
    std::vector<const TemplateDefinition *> operation_sources_; // Borrowed from workflow-owned banks; only declared independent operations.
    std::vector<const TemplateBank *> banks_;                   // Qualified local donors may lack an initialization contract.
    std::map<std::tuple<StepKind, BatchPhase, std::uint32_t>, std::vector<const TemplateDefinition *>> donors_;
};

} // namespace markov
