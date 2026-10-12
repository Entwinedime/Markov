#include "markov/adapters/frameworks/sglang/template_bindings.hpp"

#include "markov/adapters/frameworks/sglang/bindings.hpp"
#include "markov/adapters/frameworks/sglang/copy_layout.hpp"
#include "markov/adapters/frameworks/sglang/request_pool_layout.hpp"
#include "markov/adapters/runtimes/ascend/workspace_bindings.hpp"
#include "markov/graph/kernel_shapes.hpp"
#include "markov/graph/source_graph.hpp"
#include "markov/production/source_call.hpp"
#include "markov/templates/templates.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

Json read_json(const std::filesystem::path & path) {
    std::ifstream stream(path);
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    return Json::parse(stream);
}

// Resource roles come from native ownership and dependency relationships.
// Ambiguous resources retain their local identity and cannot qualify migration.
class SGLangResourceBindings {
public:
    SGLangResourceBindings(const Json & graph, const Json & instances) {
        const auto & lanes = graph.at("lanes");
        const auto & boundaries = graph.at("boundaries");
        for (const auto & lane : lanes)
            resources_.push_back({ SourceGraph::parse_domain(lane.at("domain").get<std::string>()),
                                   lane.at("rank").get<std::uint32_t>(),
                                   lane.at("native_id").get<std::uint64_t>(),
                                   std::nullopt });

        for (const auto & instance : instances) {
            const auto point = instance.at("ports").at("entered").get<std::size_t>();
            const auto lane = boundaries.at(point).at("lane").get<std::size_t>();
            if (resources_[lane].domain == SourceDomain::CpuThread) add(TemplateResourceRole::Control, lane);
        }
        for (const auto & span : graph.at("spans")) {
            if (span.at("kind") != "compute" || span.at("owner_call").is_null()) continue;
            const auto lane = boundaries.at(span.at("begin").get<std::size_t>()).at("lane").get<std::size_t>();
            add(TemplateResourceRole::Model, lane);
        }

        std::set<std::pair<std::size_t, std::size_t>> signals, submissions;
        for (const auto & edge : graph.at("edges")) {
            const auto from = boundaries.at(edge.at("source").get<std::size_t>()).at("lane").get<std::size_t>();
            const auto to = boundaries.at(edge.at("target").get<std::size_t>()).at("lane").get<std::size_t>();
            const auto & source = resources_[from];
            const auto & target = resources_[to];
            if (source.rank != target.rank) continue;

            if (edge.at("relation") == "collective_submission" && target.domain == SourceDomain::DeviceStream) add(TemplateResourceRole::Collective, to);
            if (edge.at("relation") == "native_event_signal") signals.emplace(from, to);
            if (edge.at("relation") != "native_flow") continue;

            if (source.domain == SourceDomain::CpuThread && target.domain == SourceDomain::CpuThread
                && candidates_[{ source.rank, TemplateResourceRole::Control }].contains(from) && from != to)
                add(TemplateResourceRole::Submission, to);
            if (source.domain == SourceDomain::CpuThread && target.domain == SourceDomain::DeviceStream) submissions.emplace(from, to);
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

TemplateSource SGLangTemplateBindings::produce(const std::filesystem::path & manifest_path) {
    TemplateSource result;
    result.manifest_path = std::filesystem::absolute(manifest_path).lexically_normal();

    const auto manifest = read_json(result.manifest_path);
    if (manifest.at("role") != "base" || manifest.at("framework") != "sglang") {
        throw std::invalid_argument("native dense templates require the declared SGLang base");
    }
    if (!manifest.at("graph_coverage").at("acyclic").get<bool>() || manifest.at("source_dag").is_null()) {
        throw std::invalid_argument("base source DAG is not acyclic");
    }

    const auto directory = result.manifest_path.parent_path();
    const auto prepared = read_json(directory / manifest.at("input").get<std::string>());
    result.model_path = prepared.at("model_path").get<std::string>();
    result.configuration = prepared.at("configuration");
    result.ranks = result.configuration.at("tp_size").get<std::uint32_t>();

    const auto & deployment = prepared.at("environment");
    result.deployment = { deployment.at("service").get<std::string>(), deployment.at("devices").get<std::vector<std::uint32_t>>() };
    if (result.deployment.service != "sglang-profile" || result.deployment.devices.size() != result.ranks
        || std::set(result.deployment.devices.begin(), result.deployment.devices.end()).size() != result.ranks) {
        throw std::invalid_argument("base deployment must declare one distinct device per SGLang rank");
    }

    const auto environment = read_json(directory / manifest.at("environment").get<std::string>());
    // Qualification belongs to the producer. Historical observations remain
    // useful source evidence, but cannot imply the fixed warmup contract.
    const auto initialization = environment.find("initialization");
    if (initialization == environment.end()) result.initialization_gap = "initialization_not_observed";
    else {
        const Json warmup{
            {        "input_tokens",                                                                         1 },
            {           "input_ids",                                                        Json::array({ 1 }) },
            { "sampling_parameters", { { "max_new_tokens", 2 }, { "ignore_eos", true }, { "temperature", 0 } } }
        };
        if (initialization->at("engine") != "fresh_process" || initialization->at("warmup") != warmup
            || initialization->at("workspace_snapshot") != "after_warmup_before_profile")
            result.initialization_gap = "fixed_warmup_not_observed";
    }

    result.page_size_tokens = environment.at("effective_server_args").at("page_size").get<std::uint32_t>();
    if (result.page_size_tokens == 0) throw std::invalid_argument("native KV page size must be positive");
    result.templates_path = directory / manifest.at("templates").get<std::string>();
    const auto data = read_json(result.templates_path);

    std::map<std::uint32_t, std::pair<std::int64_t, SourceAdmissionPolicy>> initial_policies;
    std::optional<AdmissionPolicy> common_policy;
    bool missing_policy = false;
    bool different_policies = false;
    std::set<std::uint32_t> observed_worker_ranks;
    for (const auto & source : data.at("instances")) {
        if (source.at("step") != "schedule") continue;
        const auto & state = source.at("source_state");
        const auto rank = source.at("rank").get<std::uint32_t>();
        const auto controls = state.find("worker_controls");
        if (controls == state.end()) {
            if (!result.source_gap)
                result.source_gap =
                    TemplateGap{ TemplateGapKind::Observation, "native_worker_controls_not_observed", StepKind::Schedule, BatchPhase::Idle, rank };
        }
        else {
            observed_worker_ranks.insert(rank);
            const auto observed = SourceWorkerControls::read(*controls);
            if (observed.waiting_timeout_seconds > 0 || observed.running_timeout_seconds > 0 || observed.test_retract || observed.require_mlp_sync)
                result.source_gap =
                    TemplateGap{ TemplateGapKind::Rule, "native_worker_control_path_not_implemented", StepKind::Schedule, BatchPhase::Idle, rank };
        }

        const auto record = state.find("admission_policy");
        if (record == state.end()) {
            missing_policy = true;
            continue;
        }

        const auto observed = SourceAdmissionPolicy::read(*record);
        if (common_policy && *common_policy != observed.parameters) different_policies = true;
        else common_policy = observed.parameters;

        // Call indices identify records, not execution order. Compare native
        // monotonic entry times within each rank, never across target clocks.
        const auto start = source.at("observed_start_ns").get<std::int64_t>();
        const auto found = initial_policies.find(rank);
        if (found == initial_policies.end() || start < found->second.first) initial_policies.insert_or_assign(rank, std::pair{ start, observed });
    }
    if (!result.source_gap && observed_worker_ranks.size() != result.ranks)
        result.source_gap = TemplateGap{ TemplateGapKind::Observation, "native_worker_controls_not_observed", StepKind::Schedule, BatchPhase::Idle, 0 };

    if (different_policies)
        result.admission_policy =
            std::unexpected(TemplateGap{ TemplateGapKind::Rule, "admission_policy_changes_not_implemented", StepKind::Schedule, BatchPhase::Idle, 0 });
    else if (!missing_policy && initial_policies.size() == result.ranks) {
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
    std::map<std::pair<std::uint32_t, std::uint64_t>, Json> publications;
    for (const auto & source : data.at("instances")) {
        if (source.at("step") != "publish") continue;
        const auto key = std::pair(source.at("rank").get<std::uint32_t>(), source.at("batch_id").get<std::uint64_t>());
        auto & records = publications.try_emplace(key, Json::array()).first->second;
        const auto & requests = source.at("source_state").at("published_requests");
        records.insert(records.end(), requests.begin(), requests.end());
    }

    const auto graph = read_json(directory / manifest.at("source_dag").get<std::string>());
    const SGLangResourceBindings resources(graph, data.at("instances"));
    const auto facts = read_json(directory / manifest.at("source_facts").get<std::string>());
    const SGLangCopyLayout copies(graph, facts);
    const bool native_path = SGLangBindings{}.supports_native_path(environment);
    const bool supported = native_path && !result.source_gap && data.at("semantic_resolution") == "native_dense_module";
    result.resolution = supported ? "native_dense_module" : "observed_subgraph";
    if (!native_path) result.source_gap = TemplateGap{ TemplateGapKind::Rule, "native_source_path_not_implemented", StepKind::Schedule, BatchPhase::Idle, 0 };
    else if (!result.source_gap && !supported)
        result.source_gap = TemplateGap{ TemplateGapKind::Rule, "native_module_rule_not_implemented", StepKind::Schedule, BatchPhase::Idle, 0 };

    for (const auto & kernel : graph.at("kernels")) {
        result.kernels.emplace(kernel.at("source_span").get<SourceSpanId>(),
                               SourceKernel{ kernel.at("name").get<std::string>(),
                                             kernel.at("operation").get<std::string>(),
                                             kernel.at("input_shapes").get<std::string>(),
                                             kernel.at("output_shapes").get<std::string>(),
                                             kernel.at("input_dtypes").get<std::string>(),
                                             kernel.at("output_dtypes").get<std::string>(),
                                             kernel.at("input_formats").get<std::string>(),
                                             kernel.at("output_formats").get<std::string>() });
    }


    for (const auto & transfer : graph.at("transfers"))
        result.transfers.emplace(transfer.at("source_span").get<SourceSpanId>(), SourceTransfer::read(transfer));

    // Preserve crossing edges before releasing the source graph. Port names alone
    // cannot represent worker-thread, stream and synchronization dependencies.
    std::map<std::size_t, std::vector<TemplateConnection>> connections;
    for (const auto & crossing : data.at("cross_template_edges")) {
        const auto edge_id = crossing.at("source_edge").get<std::size_t>();
        const auto & edge = graph.at("edges").at(edge_id);
        const auto relation = SourceGraph::parse_relation(edge.at("relation").get<std::string>());
        const auto source = crossing.at("source_boundary").get<BoundaryId>();
        const auto target = crossing.at("target_boundary").get<BoundaryId>();
        const auto sources = crossing.at("source_calls").get<std::vector<std::size_t>>();
        const auto targets = crossing.at("target_calls").get<std::vector<std::size_t>>();

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
    for (const auto & source : data.at("instances")) {
        serving_order[source.at("rank").get<std::uint32_t>()].emplace_back(source.at("observed_start_ns").get<std::int64_t>(),
                                                                           source.at("source_call").get<std::size_t>());
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
                const auto & point = graph.at("boundaries").at(*resource_boundary);
                const auto & lane = graph.at("lanes").at(point.at("lane").get<std::size_t>());
                const auto domain = SourceGraph::parse_domain(lane.at("domain").get<std::string>());
                if (domain == SourceDomain::CpuThread || domain == SourceDomain::DeviceStream) {
                    connection.resource = TemplateResource{ domain, lane.at("rank").get<std::uint32_t>(), lane.at("native_id").get<std::uint64_t>() };
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

    const RequestPoolLayout request_pool(result.configuration.at("max_running_requests").get<std::uint32_t>(), result.kernels, graph.at("operations"));
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


    const auto & calls = data.at("calls");
    std::vector<std::vector<std::size_t>> children(calls.size());
    for (std::size_t index = 0; index < calls.size(); ++index) {
        const auto & parent = calls[index].at("parent");
        if (!parent.is_null()) children[parent.get<std::size_t>()].push_back(index);
    }
    const ServingContract contract;
    for (const auto & source : data.at("instances")) {
        const auto step = contract.block(source.at("step").get<std::string>()).kind;
        const auto phase = source.at("phase").is_null() ? BatchPhase::Idle : source.at("phase") == "prefill" ? BatchPhase::Prefill : BatchPhase::Decode;
        const auto rank = source.at("rank").get<std::uint32_t>();
        const auto key = std::tuple(step, phase, rank);
        auto & entry = result.definitions[key];
        const auto & transforms = source.at("allowed_transforms");
        if (std::find(transforms.begin(), transforms.end(), "request_identity") == transforms.end()) { continue; }

        TemplateDefinition definition{ step, phase, rank, source.at("source_call").get<std::size_t>() };
        definition.entry = source.at("ports").at("entered").get<BoundaryId>();
        if (!source.at("batch_id").is_null()) definition.source_batch = source.at("batch_id").get<std::uint64_t>();
        definition.source_order = source_orders.at(definition.source_call);
        SGLangTemplateBindings bindings(definition, result.model_rules, result.collective_rules, result.attention_rules);
        bindings.bind_membership(source.at("source_state"));
        definition.binding = source.at("binding").get<std::string>();
        for (const auto & request : source.at("requests")) {
            definition.source_requests.push_back({ request.at("request_id").get<std::string>(),
                                                   request.at("input_tokens").get<std::uint32_t>(),
                                                   request.at("context_tokens").get<std::uint32_t>(),
                                                   request.at("prompt_tokens").get<std::uint32_t>(),
                                                   request.at("generated_tokens").get<std::uint32_t>() });
        }

        const Json * publication = nullptr;
        if (definition.source_batch) {
            const auto found = publications.find({ rank, *definition.source_batch });
            if (found != publications.end()) publication = &found->second;
        }
        bindings.bind_publication(publication, source.at("source_state"));

        definition.source_subtree.push_back(definition.source_call);
        for (std::size_t position = 0; position < definition.source_subtree.size(); ++position) {
            const auto & descendants = children[definition.source_subtree[position]];
            for (const auto child : descendants) definition.source_subtree.push_back(child);
        }

        for (const auto port : contract.block(step).exits) {
            definition.ports.emplace(port, source.at("ports").at(std::string(contract.port_name(port))).get<std::size_t>());
        }

        const auto & layout = source.at("input_layout");
        if (!layout.is_null()) {
            definition.input_layout = SourceTensorLayout{ layout.at("shape").get<std::vector<std::int64_t>>(),
                                                          layout.at("stride").get<std::vector<std::int64_t>>(),
                                                          layout.at("dtype").get<std::string>(),
                                                          layout.at("device").get<std::string>() };
        }

        definition.read_structure(source, graph, data.at("regions"), calls, result.kernels, std::move(connections[definition.source_call]));
        resources.bind(definition);
        for (const auto & node : definition.graph) {
            const auto transfer = result.transfers.find(definition.observation(node).source_span);
            if (transfer != result.transfers.end()) definition.observation(node).transfer = &transfer->second;
        }

        bindings.bind(graph, data.at("regions"), calls, copies, result.page_size_tokens);
        bindings.bind_request_pool(request_pool);

        bindings.bind_kv(result.page_size_tokens);
        bindings.bind_length_domain(result.page_size_tokens, result.sequence_capacity_tokens);
        // Framework semantics choose regions; the core shares exact local topology
        // without interpreting module names or changing external connections.
        std::vector<std::size_t> shared_regions;
        for (std::size_t index = 0; index < definition.regions.size(); ++index) {
            const auto & region = definition.regions[index];
            // These sibling module scopes are disjoint; nested attention_core
            // stays inside its attention owner instead of being shared twice.
            if (region.module && (region.module->semantic == "feed_forward" || region.module->semantic == "attention")) shared_regions.push_back(index);
        }
        definition.graph.share(definition, shared_regions, result.region_structures);

        entry.push_back(std::move(definition));
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
    if (supported && !result.workspace_binding_gap) AscendWorkspaceBindings{}.bind_demands(result, facts, environment);

    result.transforms = { "request_identity", "request_pool_rows", "kv_free_pages", "fixed_membership_prefill_lengths" };
    result.limits = { "fixed Qwen3 dense model, TP, backend, dtype and native deployment",
                      "separate unchunked prefill/decode without prefix cache or graph",
                      "observed variants, cold multi-token prefill within one page per member at observed batch size, and the guarded request-pool row "
                      "decrease are implemented",
                      "request-pool row rewriting preserves node topology; native kernel-variant equivalence still requires qualification",
                      "source token-shape matches do not prove identical control branches, publication effects or internal kernel variants",
                      "source dependency coverage remains partial; initial modeled resources have no in-flight work; given node durations do not establish "
                      "performance qualification" };
    return result;
}

} // namespace markov
