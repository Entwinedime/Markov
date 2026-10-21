#include "markov/adapters/frameworks/sglang/template_bindings.hpp"

#include "markov/contracts/json_input.hpp"

#include "markov/adapters/frameworks/sglang/bindings.hpp"
#include "markov/adapters/frameworks/sglang/copy_layout.hpp"
#include "markov/adapters/frameworks/sglang/request_pool_layout.hpp"
#include "markov/adapters/runtimes/ascend/source_extractor.hpp"
#include "markov/adapters/runtimes/ascend/workspace_bindings.hpp"
#include "markov/graph/kernel_shapes.hpp"
#include "markov/graph/source_graph.hpp"
#include "markov/production/source_subgraph.hpp"
#include "markov/templates/templates.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace markov {
namespace {

using Json = nlohmann::json;

using input::read_json;

bool warmup_preserves_initial_workspace(const Json & warmup, const Json & states, std::uint32_t capacity) {
    using PoolKey = std::tuple<std::uint32_t, std::uint64_t, std::int64_t, std::uint64_t>;
    const auto capacities = [](const Json & windows) {
        std::map<PoolKey, std::uint64_t> result;
        for (const auto & window : windows)
            for (const auto & pool : window.at("initial"))
                result.emplace(PoolKey{ window.at("rank"), window.at("pid"), pool.at("device"), pool.at("stream_handle") }, pool.at("capacity_bytes"));
        return result;
    };

    // Each recorded cohort is a complete prefix of the common warmup recipe.
    // Compare retained capacities, not addresses or raw profiler observations.
    const auto initial = capacities(states);
    std::set<std::uint32_t> observed;
    for (const auto & cohort : warmup.at("workspace")) {
        if (capacities(cohort.at("states")) != initial) return false;
        observed.insert(cohort.at("request_capacity").get<std::uint32_t>());
    }

    // The existing row-shrink domain describes every smaller capacity. A
    // partially observed warmup domain cannot qualify that whole interval.
    return observed.size() == capacity && *observed.begin() == 1 && *observed.rbegin() == capacity;
}

// Resource roles come from native ownership and dependency relationships.
// Ambiguous resources retain their local identity and cannot qualify migration.
class SGLangResourceBindings {
public:
    SGLangResourceBindings(const SourceGraph & graph, std::span<const BlockSubgraph> blocks, std::span<const SourceCall> calls) {
        const auto & lanes = graph.lanes();
        const auto & boundaries = graph.boundaries();
        for (const auto & lane : lanes) resources_.push_back({ lane.domain, lane.rank, lane.native_id });

        for (const auto & block : blocks) {
            const auto point = graph.spans().at(calls[block.call].marker.value()).begin;
            const auto lane = boundaries.at(point).lane;
            if (resources_[lane].domain == SourceDomain::CpuThread) add(TemplateResourceRole::Control, lane);
        }
        for (const auto & span : graph.spans()) {
            if (span.kind != SourceSpanKind::Compute || !span.owner_call) continue;
            const auto lane = boundaries.at(span.begin).lane;
            add(TemplateResourceRole::Model, lane);
        }

        const SGLangBindings bindings;
        std::set<BoundaryId> collective_cpu_points;
        for (const auto & span : graph.spans()) {
            if (resources_[boundaries[span.begin].lane].domain != SourceDomain::CpuThread) continue;
            auto scope = span.scope_call;
            while (scope) {
                const auto & call = calls[*scope];
                if (call.kind == SourceCallKind::NativeRegion && call.binding && bindings.region(*call.binding).semantic == "logical_collective") {
                    collective_cpu_points.insert(span.begin);
                    collective_cpu_points.insert(span.end);
                    break;
                }
                scope = call.parent;
            }
        }

        std::set<std::pair<std::size_t, std::size_t>> signals, submissions, collective_submissions;
        for (const auto & edge : graph.edges()) {
            const auto from = boundaries.at(edge.source).lane;
            const auto to = boundaries.at(edge.target).lane;
            const auto & source = resources_[from];
            const auto & target = resources_[to];
            if (source.rank != target.rank) continue;

            if (edge.relation == SourceRelation::CollectiveSubmission && target.domain == SourceDomain::DeviceStream) add(TemplateResourceRole::Collective, to);
            if (edge.relation == SourceRelation::NativeEventSignal) signals.emplace(from, to);
            if (edge.relation != SourceRelation::NativeFlow) continue;

            if (source.domain == SourceDomain::CpuThread && target.domain == SourceDomain::CpuThread
                && candidates_[{ source.rank, TemplateResourceRole::Control }].contains(from) && from != to)
                add(TemplateResourceRole::Submission, to);
            if (source.domain == SourceDomain::CpuThread && target.domain == SourceDomain::DeviceStream) {
                submissions.emplace(from, to);
                if (collective_cpu_points.contains(edge.source)) collective_submissions.emplace(from, to);
            }
        }

        // PG1 can retain the native event protocol without producing an HCOM
        // device task. Its stream still has an observable identity: it exchanges
        // real events with the model stream and is fed by the native CPU queue.
        // Logical TP metadata alone does not establish any physical resource.
        for (const auto & [key, models] : candidates_) {
            const auto [rank, role] = key;
            if (role != TemplateResourceRole::Model || models.size() != 1) continue;
            const auto model = *models.begin();
            for (std::size_t lane = 0; lane < resources_.size(); ++lane) {
                if (lane == model || resources_[lane].rank != rank || resources_[lane].domain != SourceDomain::DeviceStream
                    || !signals.contains({ model, lane }) || !signals.contains({ lane, model }))
                    continue;

                bool submitted = false;
                for (const auto cpu_role : { TemplateResourceRole::Control, TemplateResourceRole::Submission }) {
                    const auto cpu = candidates_.find({ rank, cpu_role });
                    if (cpu != candidates_.end() && cpu->second.size() == 1 && collective_submissions.contains({ *cpu->second.begin(), lane }))
                        submitted = true;
                }
                if (submitted) add(TemplateResourceRole::Collective, lane);
            }
        }

        for (const auto & [key, candidates] : candidates_) {
            if (candidates.size() != 1) continue;
            const auto [rank, role] = key;
            const auto lane = *candidates.begin();
            bool unique = true;
            for (const auto & [other, members] : candidates_)
                if (other != key && members.contains(lane)) unique = false;
            if (!unique) continue;

            if (role == TemplateResourceRole::Model || role == TemplateResourceRole::Collective) {
                const auto peer_role = role == TemplateResourceRole::Model ? TemplateResourceRole::Collective : TemplateResourceRole::Model;
                const auto peer = candidates_.find({ rank, peer_role });
                if (peer == candidates_.end() || peer->second.size() != 1) continue;
                const auto peer_lane = *peer->second.begin();
                if (!signals.contains({ lane, peer_lane }) || !signals.contains({ peer_lane, lane })) continue;

                if (role == TemplateResourceRole::Model) {
                    bool submitted = false;
                    for (const auto cpu_role : { TemplateResourceRole::Control, TemplateResourceRole::Submission }) {
                        const auto cpu = candidates_.find({ rank, cpu_role });
                        if (cpu != candidates_.end() && cpu->second.size() == 1 && submissions.contains({ *cpu->second.begin(), lane })) submitted = true;
                    }
                    if (!submitted) continue;
                }
            }
            roles_.emplace(resources_[lane], role);
        }
    }

    void bind(TemplateDefinition & definition) const {
        for (auto & lane : definition.lanes) lane.role = role(lane.resource());
        for (auto & wait : definition.stream_waits)
            for (auto & resource : wait.resources) resource.role = role(resource);
        for (auto & connection : definition.connection_observations)
            if (connection.resource) connection.resource->role = role(*connection.resource);
    }

private:
    void add(TemplateResourceRole role, std::size_t lane) { candidates_[{ resources_[lane].rank, role }].insert(lane); }

    std::optional<TemplateResourceRole> role(const TemplateResource & resource) const {
        const auto found = roles_.find(resource);
        if (found == roles_.end()) return std::nullopt;
        return found->second;
    }

    std::vector<TemplateResource> resources_;
    std::map<std::pair<std::uint32_t, TemplateResourceRole>, std::set<std::size_t>> candidates_;
    std::map<TemplateResource, TemplateResourceRole> roles_;
};

} // namespace

TemplateSource SGLangTemplateBindings::produce(const std::filesystem::path & manifest_path, const Json & manifest, const Json & prepared,
                                               const SourceGraph & graph, const SourceSubgraphs & subgraphs, std::span<const SourceCall> calls,
                                               const Json & facts) {
    TemplateSource result;
    result.manifest_path = std::filesystem::absolute(manifest_path).lexically_normal();

    const auto directory = result.manifest_path.parent_path();
    result.configuration = prepared.at("configuration");
    result.source_rank = prepared.at("shard_execution").at("rank").get<std::uint32_t>();
    const auto ranks = result.configuration.at("tp_size").get<std::uint32_t>();
    constexpr std::uint32_t workers = 1;
    for (std::uint32_t rank = 0; rank < ranks; ++rank) {
        // All TP workers return before the next loop's blocking request
        // broadcast. Only the entry rank makes the current output visible.
        result.rank_bindings.push_back({
            rank == 0 ? 0U : 1U,
            { PortKind::Selected, PortKind::Prepared, PortKind::Submitted, PortKind::DeviceDone, PortKind::ResultProcessed },
            rank == 0,
            rank == 0
        });
    }

    const auto & deployment = prepared.at("environment");
    result.deployment = Environment::read(deployment);
    if (result.deployment.image_id.empty()) throw std::invalid_argument("base immutable implementation identity must be recorded");
    if (result.deployment.devices.size() != workers) { throw std::invalid_argument("base deployment must declare its single physical shard device"); }

    const auto environment = read_json(directory / manifest.at("environment").get<std::string>());
    // Unknown conditions are explicit null values in the observed environment;
    // declared input is never substituted for an actual observation.
    result.deployment.measurement = environment.at("runtime").at("measurement_environment");
    result.model_configuration = environment.at("model_configuration");
    // The session observes worker warmup and first use. This producer consumes
    // that qualification; matching a request recipe alone cannot prove steady state.
    const auto & initialization = environment.at("initialization");
    const auto & warmup = initialization.at("warmup");
    if (initialization.at("engine") != "fresh_process" || warmup.at("strategy") != "paged_extent_bins" || warmup.at("logical_kv") != "empty"
        || warmup.at("in_flight") != false || initialization.at("workspace_snapshot") != "after_warmup_before_profile")
        result.steady_state_gap = "steady_initial_state_not_observed";
    else if (environment.at("steady_state").at("status") != "steady") result.steady_state_gap = environment.at("steady_state").at("status").get<std::string>();

    result.page_size_tokens = environment.at("effective_server_args").at("page_size").get<std::uint32_t>();
    if (result.page_size_tokens == 0) throw std::invalid_argument("native KV page size must be positive");

    std::map<std::uint32_t, std::pair<std::int64_t, SourceAdmissionPolicy>> initial_policies;
    std::optional<AdmissionPolicy> common_policy;
    bool missing_policy = false;
    bool different_policies = false;
    std::set<std::uint32_t> observed_worker_ranks;
    for (const auto & call : calls) {
        if (call.is_annotation() || call.step != StepKind::Schedule) continue;
        if (call.rank != result.source_rank) throw std::invalid_argument("native worker observations disagree with the captured logical role");
        const auto rank = call.rank;
        // These flags have meaning only in SGLang. Read their original facts
        // here; the shared source graph has no second parsed copy of them.
        const auto & metadata = facts.at("calls").at(call.identity).at("metadata");
        const auto qualification = SGLangBindings{}.qualify_worker_controls(metadata);
        if (qualification == SGLangBindings::WorkerControlQualification::NotObserved) {
            if (!result.source_gap)
                result.source_gap =
                    TemplateGap{ TemplateGapKind::Observation, "native_worker_controls_not_observed", StepKind::Schedule, BatchPhase::Idle, rank };
        }
        else {
            observed_worker_ranks.insert(rank);
            if (qualification == SGLangBindings::WorkerControlQualification::Unsupported)
                result.source_gap =
                    TemplateGap{ TemplateGapKind::Rule, "native_worker_control_path_not_implemented", StepKind::Schedule, BatchPhase::Idle, rank };

            const auto & controls = metadata.at("worker_controls");
            static constexpr std::array role_fields{ "physical_tp_size",  "physical_tp_rank", "scheduler_tp_size",
                                                     "scheduler_tp_rank", "receiver_tp_size", "receiver_tp_rank" };
            const bool observed = std::ranges::all_of(role_fields, [&](const auto field) { return controls.contains(field) && !controls.at(field).is_null(); });
            if (!observed && !result.source_gap)
                result.source_gap = TemplateGap{ TemplateGapKind::Observation, "native_worker_role_not_observed", StepKind::Schedule, BatchPhase::Idle, rank };
            else if (observed
                     && (controls.at("physical_tp_size") != workers || controls.at("physical_tp_rank") != 0 || controls.at("scheduler_tp_size") != ranks
                         || controls.at("receiver_tp_size") != ranks || controls.at("scheduler_tp_rank") != rank || controls.at("receiver_tp_rank") != rank))
                result.source_gap = TemplateGap{ TemplateGapKind::Rule, "native_worker_role_not_qualified", StepKind::Schedule, BatchPhase::Idle, rank };
        }

        if (!call.admission_policy) {
            missing_policy = true;
            continue;
        }

        const auto & observed = *call.admission_policy;
        if (common_policy && *common_policy != observed.parameters) different_policies = true;
        else common_policy = observed.parameters;

        // Call indices identify records, not execution order. Compare native
        // monotonic entry times within each rank, never across target clocks.
        const auto start = call.observed_start_ns;
        const auto found = initial_policies.find(rank);
        if (found == initial_policies.end() || start < found->second.first) initial_policies.insert_or_assign(rank, std::pair{ start, observed });
    }
    if (!result.source_gap && observed_worker_ranks.size() != workers)
        result.source_gap = TemplateGap{ TemplateGapKind::Observation, "native_worker_controls_not_observed", StepKind::Schedule, BatchPhase::Idle, 0 };

    if (different_policies)
        result.admission_policy =
            std::unexpected(TemplateGap{ TemplateGapKind::Rule, "admission_policy_changes_not_implemented", StepKind::Schedule, BatchPhase::Idle, 0 });
    else if (!missing_policy && initial_policies.size() == workers) {
        result.admission_policy = *common_policy;
        for (const auto & [rank, entry] : initial_policies) {
            if (entry.second.entry_ratio == entry.second.parameters.initial_ratio) continue;
            result.admission_policy =
                std::unexpected(TemplateGap{ TemplateGapKind::Rule, "admission_initial_state_not_cold", StepKind::Schedule, BatchPhase::Idle, rank });
            break;
        }
    }

    // Publication records live only while producing definitions. Runtime branch
    // checks receive slot-ordered facts rather than a native identity lookup.
    std::map<std::pair<std::uint32_t, std::uint64_t>, std::vector<SourcePublication>> publications;
    for (const auto & call : calls) {
        if (call.is_annotation() || call.step != StepKind::Publish) continue;
        auto & records = publications[{ call.rank, call.batch_id.value() }];
        records.insert(records.end(), call.publications.begin(), call.publications.end());
    }

    const SGLangResourceBindings resources(graph, subgraphs.blocks, calls);
    const SGLangCopyLayout copies(graph, facts);
    const bool native_path = SGLangBindings{}.supports_native_path(environment, workers);
    result.resolution = native_path ? "native_dense_module" : "observed_subgraph";
    if (!native_path) result.source_gap = TemplateGap{ TemplateGapKind::Rule, "native_source_path_not_implemented", StepKind::Schedule, BatchPhase::Idle, 0 };

    result.kernels = graph.kernels();
    result.transfers = graph.transfers();

    // Preserve crossing edges before releasing the source graph. Port names alone
    // cannot represent worker-thread, stream and synchronization dependencies.
    std::map<std::size_t, std::vector<TemplateConnection>> connections;
    for (const auto & crossing : subgraphs.crossings) {
        const auto edge_id = crossing.edge;
        const auto & edge = graph.edges().at(edge_id);
        const auto relation = edge.relation;
        const auto source = edge.source;
        const auto target = edge.target;
        const auto & sources = crossing.sources;
        const auto & targets = crossing.targets;

        // A shared owner already consumes this edge internally. Only owners
        // on one side need an external connection; keep all remote identities.
        for (const auto owner : sources) {
            if (std::find(targets.begin(), targets.end(), owner) != targets.end()) continue;
            connections[owner].push_back({ edge_id, relation, TemplateConnectionDirection::Outgoing, source, target, targets, std::nullopt, std::nullopt });
        }
        for (const auto owner : targets) {
            if (std::find(sources.begin(), sources.end(), owner) != sources.end()) continue;
            connections[owner].push_back({ edge_id, relation, TemplateConnectionDirection::Incoming, target, source, sources, std::nullopt, std::nullopt });
        }
    }

    // Ordering qualifies native imports within a rank, including earlier stages
    // of the same batch. Target connections use invocation order, never these times.
    std::map<std::uint32_t, std::vector<std::pair<std::int64_t, std::size_t>>> serving_order;
    for (const auto & block : subgraphs.blocks) {
        const auto & call = calls[block.call];
        serving_order[call.rank].emplace_back(call.observed_start_ns, block.call);
    }
    std::map<std::size_t, std::uint64_t> source_orders;
    for (auto & [rank, calls] : serving_order) {
        std::ranges::sort(calls);
        for (std::size_t order = 0; order < calls.size(); ++order) source_orders.emplace(calls[order].second, order);
    }
    for (auto & [owner, entries] : connections) {
        for (auto & connection : entries) {
            const auto incoming = connection.direction == TemplateConnectionDirection::Incoming;
            std::optional<BoundaryId> resource_boundary;
            if (connection.relation == SourceRelation::ThreadOrder) resource_boundary = connection.local_boundary;
            if (connection.relation == SourceRelation::StreamOrder)
                resource_boundary = incoming ? connection.local_boundary : connection.remote_source_boundary;
            // A host wait imports or exports the device completion frontier,
            // never the waiting CPU thread as a device resource.
            if (connection.relation == SourceRelation::NativeHostWait)
                resource_boundary = incoming ? connection.remote_source_boundary : connection.local_boundary;
            if (resource_boundary) {
                const auto & point = graph.boundaries().at(*resource_boundary);
                const auto & lane = graph.lanes().at(point.lane);
                const auto domain = lane.domain;
                if (domain == SourceDomain::CpuThread || domain == SourceDomain::DeviceStream) {
                    connection.resource = TemplateResource{ domain, lane.rank, lane.native_id };
                }
            }

            for (const auto remote : connection.remote_source_calls) {
                const auto found = source_orders.find(remote);
                if (found == source_orders.end() || (connection.remote_order && *connection.remote_order != found->second)) {
                    connection.remote_order.reset();
                    break;
                }
                connection.remote_order = found->second;
            }
        }
    }

    const RequestPoolLayout request_pool(result.configuration.at("max_running_requests").get<std::uint32_t>(), result.kernels, graph.operations());
    const auto request_capacity = SGLangBindings{}.request_capacity(environment);
    result.request_pool_domain = request_pool.capacity_domain();
    if (const auto table_tokens = request_pool.sequence_capacity()) {
        const auto & model = environment.at("model_configuration");
        const auto & configured_context = environment.at("effective_server_args").at("context_length");
        auto capacity = std::min(
            { *table_tokens, model.at("max_position_embeddings").get<std::int64_t>(), result.configuration.at("max_total_tokens").get<std::int64_t>() });
        if (!configured_context.is_null()) capacity = std::min(capacity, configured_context.get<std::int64_t>());

        // Fixed backing storage is distinct from the observed request lengths.
        // KV uses the logical budget, excluding the allocator's extra physical page.
        for (const auto & [span, kernel] : result.kernels) {
            if (kernel.operation != "RopeWithSinCosCache") continue;
            const KernelShapes inputs(kernel.input_shapes);
            capacity = std::min(capacity, inputs.tensors.at(3).at(0));
        }
        if (capacity <= 0) throw std::invalid_argument("native sequence capacity must be positive");
        result.sequence_capacity_tokens = static_cast<std::uint32_t>(capacity);
    }


    for (const auto & block : subgraphs.blocks) {
        const auto & call = calls[block.call];
        auto definition = block.build(graph, calls, subgraphs.regions, result.kernels, std::move(connections[block.call]));
        definition.source_order = source_orders.at(block.call);
        SGLangTemplateBindings bindings(graph, definition, result.model_rules, result.attention_rules);
        bindings.bind_membership(call.batch_filters);

        std::span<const SourcePublication> publication;
        if (call.batch_id) {
            const auto found = publications.find({ call.rank, *call.batch_id });
            if (found != publications.end()) publication = found->second;
        }
        bindings.bind_publication(publication, call.output_policy);

        resources.bind(definition);
        for (const auto & node : definition.graph) {
            const auto transfer = result.transfers.find(definition.observation(node).source_span);
            if (transfer != result.transfers.end()) definition.observation(node).transfer = &transfer->second;
            if (node.kind == SourceSpanKind::DeviceMarker)
                definition.observation(node).device_command =
                    AscendSourceExtractor::device_command(facts.at("intervals").at(definition.observation(node).source_span));
        }

        bindings.bind(calls, facts.at("calls"), copies, result.page_size_tokens, request_capacity);
        bindings.bind_model_sequence(result.model_configuration.at("num_hidden_layers").get<std::uint32_t>());
        const auto parallel = bindings.bind_collective_interfaces(ranks, calls, facts);
        if (!parallel && !result.source_gap) result.source_gap = parallel.error();
        bindings.bind_request_pool(request_pool);

        bindings.bind_kv(result.page_size_tokens);
        bindings.bind_length_domain(result.page_size_tokens, result.sequence_capacity_tokens);
        bindings.finalize_regions();
        // Declared native regions supply semantic identity. The common graph
        // partitions nested ownership and shares parent/child composition.
        std::vector<std::size_t> shared_regions;
        for (std::size_t index = 0; index < definition.regions.size(); ++index) {
            const auto & region = definition.regions[index];
            if (!region.semantic.empty() && region.cpu_scope) shared_regions.push_back(index);
        }
        definition.graph.share(definition, shared_regions, result.region_structures);

        result.definitions[{ definition.step, definition.phase, definition.rank }].push_back(std::move(definition));
    }

    result.gaps = manifest.at("gaps");
    for (const auto & gap : manifest.at("extraction_gaps")) result.gaps.push_back(gap);
    for (const auto & gap : result.gaps) {
        if (gap.at("kind") == "workspace") {
            result.workspace_binding_gap = gap.at("reason").get<std::string>();
            break;
        }
    }
    if (!result.workspace_binding_gap) AscendWorkspaceBindings{}.bind(result, facts, graph);
    const bool supported = native_path && !result.source_gap;
    if (supported && !result.workspace_binding_gap) AscendWorkspaceBindings{}.bind_demands(result, facts, environment);
    // Repetition compares the completed work bindings, including allocator
    // state and demand rules added by the runtime adapter.
    for (auto & [key, definitions] : result.definitions)
        for (auto & definition : definitions) definition.qualify_sequences();

    result.initialization_pool_domain = { result.request_pool_domain.maximum, "workspace_initial_capacity_migration_not_qualified" };
    if (supported && !result.steady_state_gap && !result.workspace_binding_gap && !result.request_pool_domain.shrink_gap
        && warmup_preserves_initial_workspace(environment.at("initialization").at("warmup"), facts.at("workspace_states"), result.request_pool_domain.maximum))
        result.initialization_pool_domain = result.request_pool_domain;

    result.transforms = { "request_identity", "request_pool_rows", "kv_free_pages", "qualified_prefill_lengths", "qualified_batch_membership" };
    result.limits = {
        "fixed Qwen3 dense model configuration, TP, backend, dtype and immutable native implementation; card indices are capture locations",
        "separate unchunked prefill/decode without prefix cache or graph",
        "cold multi-token prefill within one page per member; request membership, filter/merge and shape changes require qualified template bindings",
        "request-pool row rewriting preserves node topology; native kernel-variant equivalence still requires qualification",
        "source token-shape matches do not prove identical control branches, publication effects or internal kernel variants",
        "source dependency coverage remains partial; initial modeled resources have no in-flight work; given node durations do not establish "
        "performance qualification"
    };
    return result;
}

} // namespace markov
