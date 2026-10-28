#include "markov/adapters/frameworks/sglang/structure.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <utility>

namespace markov::sglang {

ServingStructure::ServingStructure(RegionGraph & graph, std::span<const TemplateAsset> assets, ServingSettings settings, std::span<const Request> requests,
                                   const ServingTopology & topology, const std::string & instance, const nlohmann::json & routes)
    : graph_(graph),
      topology_(topology),
      instance_(topology.instance(instance)),
      parallel_(graph_),
      prefill_token_granularity_(settings.page_size) {
    for (const auto rank : instance_.members) {
        const auto source = std::ranges::find_if(assets, [&](const auto & asset) { return asset.environment().at("rank") == rank; });
        assets_.push_back(&*source);
    }
    RankStructure::check_target(*assets_.front(), settings, requests, instance != "prefill");

    for (const auto * asset : assets_) {
        const auto & environment = asset->environment();
        if (environment.at("workspace_policy").at("kind") != "native_attention_bound_reservation")
            throw std::invalid_argument("SGLang serving requires its declared native workspace warmup");
        for (const auto * key : { "implementation", "torch", "torch_npu", "cann", "device", "source_transport" }) {
            if (environment.at(key) != assets_.front()->environment().at(key))
                throw std::invalid_argument("rank sources have different implementation domains");
        }
    }

    const bool experts = std::ranges::any_of(assets_, [](const auto * asset) {
        return std::ranges::any_of(asset->entrypoints(), [](const auto & entry) { return entry.role == "expert_layer"; });
    });
    if (experts) {
        if (routes.is_null()) throw std::invalid_argument("MoE targets require an EP topology and matching workload routes");
        routing_.emplace(assets_, requests, routes);
    }
    else if (!routes.is_null()) throw std::invalid_argument("EP routes require native expert-layer sources");

    for (const auto * source : assets_) {
        const auto offset = graph_.execution_resources.size();
        const auto & asset = *source;
        const auto rank = asset.environment().at("rank").get<std::size_t>();
        const auto & resources = asset.execution_resources();
        graph_.execution_resources.insert(graph_.execution_resources.end(), resources.begin(), resources.end());
        if (asset.environment().at("source_transport") == "native_disaggregation") {
            for (std::size_t resource = 0; resource < resources.size(); ++resource) {
                if (resources[resource] == "stream" || resources[resource] == "thread") graph_.execution.initially_idle(offset + resource);
            }
        }
        ranks_.emplace(
            rank,
            std::make_unique<RankStructure>(asset, settings, graph_, offset, rank, topology_.stage_count(instance), routing_ ? &*routing_ : nullptr));
    }
}

GroupAdmission ServingStructure::admission() const {

    // PrefillAdder._update_prefill_budget charges whole pages on the fixed-output, cold-cache path.
    GroupAdmission result;
    std::vector<std::size_t> occupied;
    for (std::size_t lane = 0; lane < topology_.stage_count(instance_.id); ++lane) occupied.push_back(ranks_.begin()->second->admission_members(lane));
    result.emplace(instance_.id, AdmissionConstraints{ std::move(occupied), prefill_token_granularity_ });
    return result;
}

BatchProgram ServingStructure::expand(const ServingAction & action) { return expand(action, {}); }

BatchProgram ServingStructure::expand(const ServingAction & action, const std::map<std::size_t, std::vector<std::size_t>> & prerequisites) {
    const auto & group = topology_.parallel().group(action.task.group);
    const auto operation_begin = graph_.operations.size();
    const auto node_begin = graph_.execution.nodes().size();
    const auto dependency_begin = graph_.execution.dependencies().size();
    const auto region_begin = graph_.regions.size();
    std::vector<RankWork> work;
    BatchProgram result{ std::nullopt, {}, {} };
    if (action.step == ServingStep::Stage) result = pipeline_stage(action, work);
    else if (action.step == ServingStep::ReadResult) result = read_result(action, work);
    else if (action.step == ServingStep::Result) result = pipeline_result(action, work);
    else if ((action.step == ServingStep::Batch || action.step == ServingStep::Model) && topology_.stage_count(instance_.id) > 1) {

        // Simple PD control submits a whole batch; its execution still uses the shared stage and message expansion.
        auto staged = action;
        for (std::size_t stage = 0; stage < topology_.stage_count(instance_.id); ++stage) {
            staged.stage = stage;
            pipeline_stage(staged, work);
        }
        staged.stage.reset();
        result = pipeline_result(staged, work);
    }
    else {
        for (const auto rank : group.members) {
            const auto begin = graph_.operations.size();
            const auto rank_node_begin = graph_.execution.first_node() + graph_.execution.nodes().size();
            auto program = ranks_.at(rank)->expand(action);
            if (const auto required = prerequisites.find(rank); required != prerequisites.end()) {
                const auto consumers = graph_.execution.initial_nodes(rank_node_begin);
                for (const auto producer : required->second) {
                    for (const auto consumer : consumers) graph_.execution.depend(producer, consumer, "instance_input_ready");
                }
            }
            work.push_back({ rank, begin, graph_.operations.size() });
            if (program.publication) {
                if (result.publication) throw std::invalid_argument("one serving action has multiple publishing roles");
                result.publication = program.publication;
            }
            if (rank == group.members.front()) result.returned_pages = std::move(program.returned_pages);
            result.completions.push_back(program.completion);
        }
    }
    parallel_.connect(work, topology_.parallel(), action.task, action.flow);

    for (std::size_t region = region_begin; region < graph_.regions.size(); ++region) {
        graph_.regions[region]["batch"] = action.flow;
        graph_.regions[region]["action"] = action.task.sequence;
        graph_.regions[region]["lane"] = action.lane;
    }
    ++action_count_;
    if (action.step == ServingStep::Batch || action.step == ServingStep::Model || action.stage == 0) ++batch_count_;
    operation_count_ += graph_.operations.size() - operation_begin;
    node_count_ += graph_.execution.nodes().size() - node_begin;
    dependency_count_ += graph_.execution.dependencies().size() - dependency_begin;

    return result;
}

BatchProgram ServingStructure::pipeline_stage(const ServingAction & action, std::vector<RankWork> & work) {
    const auto & batch = action.work->batch;
    const auto stage = action.stage.value();
    if (stage == 0) {
        auto & flow = flows_.emplace(action.flow, FlowViews{}).first->second;

        // Every rank encodes the same allocation once; only graph ports are retained per flow.
        for (auto & [rank, builder] : ranks_) {
            const auto begin = graph_.operations.size();
            flow.inputs[rank] = builder->prepare(batch, action.lane);
            work.push_back({ rank, begin, graph_.operations.size() });
        }
    }

    auto & flow = flows_.at(action.flow);
    BatchProgram result{ std::nullopt, {}, {} };
    const auto & members = topology_.stage(instance_.id, stage).members;
    for (std::size_t peer = 0; peer < members.size(); ++peer) {
        const auto rank = members[peer];
        const auto begin = graph_.operations.size();
        const auto model = ranks_.at(rank)->model(batch, flow.inputs[rank]);
        flow.inputs[rank].clear();
        flow.outputs[rank] = model.ports.outputs;
        flow.outputs[rank].erase("token_relay");
        result.completions.push_back(graph_.execution.region_call(model.region).returned);
        work.push_back({ rank, begin, graph_.operations.size() });

        if (stage + 1 == topology_.stage_count(instance_.id)) continue;
        std::map<std::string, std::size_t> payload;
        for (const auto & [name, value] : model.ports.outputs) {
            if (name.starts_with("proxy.")) {
                payload.emplace(name.substr(6), value);
                flow.outputs[rank].erase(name);
            }
        }
        const auto send_begin = graph_.operations.size();
        const auto sent = ranks_.at(rank)->message("pipeline_send_proxy", batch, payload, true);
        ranks_.at(rank)->wait_message("proxy", batch, sent);
        work.push_back({ rank, send_begin, graph_.operations.size() });
        result.completions.push_back(graph_.execution.region_call(sent.region.region).returned);

        const auto receiver = topology_.stage(instance_.id, stage + 1).members[peer];
        const auto receive_begin = graph_.operations.size();
        const auto received = ranks_.at(receiver)->message("pipeline_receive_proxy", batch, payload, false);
        for (const auto & [name, value] : received.region.ports.outputs) flow.inputs[receiver].emplace("proxy." + name, value);
        work.push_back({ receiver, receive_begin, graph_.operations.size() });
        result.completions.push_back(graph_.execution.region_call(received.region.region).returned);
    }
    return result;
}

BatchProgram ServingStructure::read_result(const ServingAction & action, std::vector<RankWork> & work) {
    const auto & flow = flows_.at(action.flow);
    BatchProgram result{ std::nullopt, {}, {} };
    for (const auto rank : instance_.members) {
        const auto begin = graph_.operations.size();
        result.completions.push_back(ranks_.at(rank)->begin_result(action.work->batch, flow.outputs.at(rank).at("sampled_tokens")).value());
        work.push_back({ rank, begin, graph_.operations.size() });
    }
    return result;
}

BatchProgram ServingStructure::pipeline_result(const ServingAction & action, std::vector<RankWork> & work) {
    const auto & batch = action.work->batch;
    const auto & flow = flows_.at(action.flow);
    const auto tp = topology_.tensor_parallel_size(instance_.id);
    const auto stages = topology_.stage_count(instance_.id);
    BatchProgram result{ std::nullopt, {}, {} };
    std::map<std::size_t, std::size_t> completions;
    for (std::size_t tensor_rank = 0; tensor_rank < tp; ++tensor_rank) {
        const auto last = topology_.stage(instance_.id, stages - 1).members[tensor_rank];
        if (stages == 1) {

            // A local model result has no pipeline token ring.
            const auto begin = graph_.operations.size();
            const auto & outputs = flow.outputs.at(last);
            auto program = ranks_.at(last)->complete(batch, action.work->effects, action.lane, outputs, outputs.at("sampled_tokens"), false);
            if (program.publication) result.publication = program.publication;
            if (last == instance_.members.front()) result.returned_pages = std::move(program.returned_pages);
            completions.emplace(last, program.completion);
            work.push_back({ last, begin, graph_.operations.size() });
            continue;
        }
        std::map<std::string, std::size_t> payload{
            { "next_token_ids", flow.outputs.at(last).at("sampled_tokens") }
        };
        const auto send_begin = graph_.operations.size();
        const auto sent = ranks_.at(last)->message("pipeline_send_output", batch, payload, true);
        ranks_.at(last)->wait_message("output", batch, sent);
        work.push_back({ last, send_begin, graph_.operations.size() });

        // Native PP circulates sampled tokens through every stage before its local result is consumed.
        for (std::size_t stage = 0; stage < stages; ++stage) {
            const auto rank = topology_.stage(instance_.id, stage).members[tensor_rank];
            const auto begin = graph_.operations.size();
            const auto received = ranks_.at(rank)->message("pipeline_receive_output", batch, payload, false);
            payload = received.region.ports.outputs;
            const auto tokens = ranks_.at(rank)->receive_result(batch, payload.at("next_token_ids"));
            auto program =
                ranks_.at(rank)->complete(batch, action.work->effects, action.lane, flow.outputs.at(rank), tokens, action.step == ServingStep::Model);
            if (program.publication) {
                if (result.publication) throw std::invalid_argument("one serving action has multiple publishing roles");
                result.publication = program.publication;
            }
            if (rank == instance_.members.front()) result.returned_pages = std::move(program.returned_pages);
            completions.emplace(rank, program.completion);
            if (stage + 1 < stages) {
                const auto forwarded = ranks_.at(rank)->message("pipeline_send_output", batch, payload, true);
                ranks_.at(rank)->wait_message("output", batch, forwarded);
            }
            work.push_back({ rank, begin, graph_.operations.size() });
        }
    }

    // PD readiness follows instance order, independently of the token ring's traversal.
    for (const auto rank : instance_.members) result.completions.push_back(completions.at(rank));
    flows_.erase(action.flow);
    return result;
}

const RegionGraph & ServingStructure::graph() const { return graph_; }

std::vector<std::size_t> ServingStructure::idle() {
    const auto operation_begin = graph_.operations.size();
    const auto node_begin = graph_.execution.nodes().size();
    const auto dependency_begin = graph_.execution.dependencies().size();
    const auto region_begin = graph_.regions.size();
    std::vector<std::size_t> completions;
    for (auto & [_, rank] : ranks_) {
        if (const auto completion = rank->idle()) completions.push_back(*completion);
    }
    if (completions.empty()) return completions;

    for (std::size_t region = region_begin; region < graph_.regions.size(); ++region) graph_.regions[region]["control"] = control_count_;
    ++control_count_;
    operation_count_ += graph_.operations.size() - operation_begin;
    node_count_ += graph_.execution.nodes().size() - node_begin;
    dependency_count_ += graph_.execution.dependencies().size() - dependency_begin;
    return completions;
}

std::vector<std::size_t> ServingStructure::retire() {
    GraphRoots roots;
    live_state(roots);
    return graph_.retire(std::move(roots));
}

void ServingStructure::live_state(GraphRoots & roots) {
    parallel_.live_state(roots);
    for (auto & [_, rank] : ranks_) rank->live_state(roots);
    for (auto & [_, flow] : flows_) {
        for (auto & [rank, inputs] : flow.inputs) {
            for (auto & [name, value] : inputs) roots.values.push_back(&value);
        }
        for (auto & [rank, output] : flow.outputs) {
            for (auto & [name, value] : output) roots.values.push_back(&value);
        }
    }
}

PageReuse ServingStructure::page_reuse() const { return ranks_.begin()->second->page_reuse(); }
std::span<const TemplateAsset * const> ServingStructure::sources() const { return assets_; }
TargetRegion ServingStructure::transfer(std::size_t rank, std::int64_t bytes, std::int64_t segments) { return ranks_.at(rank)->transfer(bytes, segments); }

nlohmann::json ServingStructure::summary() const {
    const auto & environment = assets_.front()->environment();
    const auto & implementation = environment.at("implementation");
    nlohmann::json result{
        {         "framework",environment.at("framework")                              },
        {             "model",              implementation.at("model_path") },
        {             "dtype",                   implementation.at("dtype") },
        { "attention_backend",       implementation.at("attention_backend") },
        {        "logical_tp", topology_.tensor_parallel_size(instance_.id) },
        {        "logical_pp",          topology_.stage_count(instance_.id) },
        {        "logical_ep",                 environment.at("logical_ep") },
        {          "topology",              topology_.parallel().document() },
        {            "counts",
         { { "batches", batch_count_ },
         { "actions", action_count_ },
         { "control_decisions", control_count_ },
         { "operations", operation_count_ },
         { "execution_nodes", node_count_ },
         { "execution_dependencies", dependency_count_ } }                 },
        {      "rank_sources",                      nlohmann::json::array() }
    };
    for (const auto & [_, rank] : ranks_) result["rank_sources"].push_back(rank->source_summary());

    // shortcut: rejected-candidate retries and wire progress need their own native protocol evidence.
    result["qualification"] = {
        {     "operand_contents",                        "control_indices_bound" },
        { "release_index_source",                             "request_map_view" },
        {         "token_values",                                     "symbolic" },
        {          "preparation",                           "native_batch_calls" },
        {    "batch_transitions",                          "native_column_calls" },
        {           "scheduling",                 "native_scheduled_batch_calls" },
        {   "resource_execution", "observed_relations_and_collective_interfaces" },
        {            "workspace",             "prewarmed_native_attention_bound" }
    };
    if (routing_) result["qualification"]["expert_routes"] = "given_workload_condition";
    return result;
}

nlohmann::json ServingStructure::document() const {
    auto result = graph_.document();
    result["scope"] = "serving_structure";
    result["environment"] = assets_.front()->environment();
    result["environment"].erase("rank");
    result["topology"] = topology_.parallel().document();
    return result;
}

} // namespace markov::sglang
