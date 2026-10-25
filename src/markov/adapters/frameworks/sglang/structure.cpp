#include "markov/adapters/frameworks/sglang/structure.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <utility>

namespace markov::sglang {

ServingStructure::ServingStructure(std::span<const TemplateAsset> assets, ServingSettings settings, std::span<const Request> requests,
                                   const ServingTopology & topology, const nlohmann::json & routes)
    : topology_(topology),
      parallel_(graph_),
      prefill_token_granularity_(settings.page_size),
      assets_(assets.size()) {
    if (assets.empty()) throw std::invalid_argument("serving requires rank assets");
    RankStructure::check_target(assets.front(), settings, requests, true);

    for (const auto & asset : assets) {
        const auto & environment = asset.environment();
        if (environment.at("workspace_policy").at("kind") != "native_attention_bound_reservation")
            throw std::invalid_argument("SGLang serving requires its declared native workspace warmup");
        const auto rank = environment.at("rank").get<std::size_t>();
        for (const auto * key : { "implementation", "torch", "torch_npu", "cann", "device", "source_transport" }) {
            if (environment.at(key) != assets.front().environment().at(key)) throw std::invalid_argument("rank sources have different implementation domains");
        }
        assets_[rank] = &asset;
    }

    const bool experts = std::ranges::any_of(assets, [](const auto & asset) {
        return std::ranges::any_of(asset.entrypoints(), [](const auto & entry) { return entry.role == "expert_layer"; });
    });
    if (experts) {
        if (routes.is_null()) throw std::invalid_argument("MoE targets require an EP topology and matching workload routes");
        routing_.emplace(assets, requests, routes);
    }
    else if (!routes.is_null()) throw std::invalid_argument("EP routes require native expert-layer sources");

    for (std::size_t rank = 0; rank < assets_.size(); ++rank) {
        const auto offset = graph_.execution_resources.size();
        const auto & asset = *assets_[rank];
        const auto & resources = asset.execution_resources();
        graph_.execution_resources.insert(graph_.execution_resources.end(), resources.begin(), resources.end());
        ranks_.push_back(std::make_unique<RankStructure>(asset, settings, graph_, offset, rank, topology_.stage_count(), routing_ ? &*routing_ : nullptr));
    }
}

GroupAdmission ServingStructure::admission() const {

    // PrefillAdder._update_prefill_budget charges whole pages on the fixed-output, cold-cache path.
    GroupAdmission result;
    std::vector<std::size_t> occupied;
    for (std::size_t lane = 0; lane < topology_.stage_count(); ++lane) occupied.push_back(ranks_.front()->admission_members(lane));
    result.emplace("model", AdmissionConstraints{ std::move(occupied), prefill_token_granularity_ });
    return result;
}

BatchProgram ServingStructure::expand(const ServingAction & action) {
    const auto & group = topology_.parallel().group(action.task.group);
    const auto operation_begin = graph_.operations.size();
    const auto node_begin = graph_.execution.nodes().size();
    const auto dependency_begin = graph_.execution.dependencies().size();
    const auto region_begin = graph_.regions.size();
    std::vector<RankWork> work;
    BatchProgram result{ std::nullopt, {}, {} };
    if (action.step == ServingStep::Stage) result = pipeline_stage(action, work);
    else if (action.step == ServingStep::Result) result = pipeline_result(action, work);
    else {
        for (const auto rank : group.members) {
            const auto begin = graph_.operations.size();
            auto program = ranks_[rank]->expand(action.work->batch, action.work->effects);
            work.push_back({ rank, begin, graph_.operations.size() });
            if (program.publication) {
                if (result.publication) throw std::invalid_argument("one serving action has multiple publishing roles");
                result.publication = program.publication;
            }
            if (rank == group.members.front()) result.returned_pages = std::move(program.returned_pages);
            result.completions.push_back(program.completion);
        }
    }
    parallel_.connect(work, topology_.parallel(), action.task);

    for (std::size_t region = region_begin; region < graph_.regions.size(); ++region) {
        graph_.regions[region]["batch"] = action.flow;
        graph_.regions[region]["action"] = action.task.sequence;
        graph_.regions[region]["lane"] = action.lane;
    }
    ++action_count_;
    if (action.step == ServingStep::Batch || action.stage == 0) ++batch_count_;
    operation_count_ += graph_.operations.size() - operation_begin;
    node_count_ += graph_.execution.nodes().size() - node_begin;
    dependency_count_ += graph_.execution.dependencies().size() - dependency_begin;

    return result;
}

BatchProgram ServingStructure::pipeline_stage(const ServingAction & action, std::vector<RankWork> & work) {
    const auto & batch = action.work->batch;
    const auto stage = action.stage.value();
    if (stage == 0) {
        auto & flow = flows_
                          .emplace(action.flow,
                                   FlowViews{ std::vector<std::map<std::string, std::size_t>>(ranks_.size()),
                                              std::vector<std::map<std::string, std::size_t>>(ranks_.size()) })
                          .first->second;

        // Every rank encodes the same allocation once; only graph ports are retained per flow.
        for (std::size_t rank = 0; rank < ranks_.size(); ++rank) {
            const auto begin = graph_.operations.size();
            flow.inputs[rank] = ranks_[rank]->prepare(batch, action.lane);
            work.push_back({ rank, begin, graph_.operations.size() });
        }
    }

    auto & flow = flows_.at(action.flow);
    BatchProgram result{ std::nullopt, {}, {} };
    for (const auto rank : topology_.stage(stage).members) {
        const auto begin = graph_.operations.size();
        const auto model = ranks_[rank]->model(batch, flow.inputs[rank]);
        flow.inputs[rank].clear();
        flow.outputs[rank] = model.ports.outputs;
        flow.outputs[rank].erase("token_relay");
        result.completions.push_back(graph_.execution.region_call(model.region).returned);
        work.push_back({ rank, begin, graph_.operations.size() });

        if (stage + 1 == topology_.stage_count()) continue;
        std::map<std::string, std::size_t> payload;
        for (const auto & [name, value] : model.ports.outputs) {
            if (name.starts_with("proxy.")) {
                payload.emplace(name.substr(6), value);
                flow.outputs[rank].erase(name);
            }
        }
        const auto send_begin = graph_.operations.size();
        const auto sent = ranks_[rank]->message("pipeline_send_proxy", batch, payload, true);
        ranks_[rank]->wait_message("proxy", batch, sent);
        work.push_back({ rank, send_begin, graph_.operations.size() });
        result.completions.push_back(graph_.execution.region_call(sent.region.region).returned);

        const auto receiver = rank + topology_.tensor_parallel_size();
        const auto receive_begin = graph_.operations.size();
        const auto received = ranks_[receiver]->message("pipeline_receive_proxy", batch, payload, false);
        for (const auto & [name, value] : received.region.ports.outputs) flow.inputs[receiver].emplace("proxy." + name, value);
        work.push_back({ receiver, receive_begin, graph_.operations.size() });
        result.completions.push_back(graph_.execution.region_call(received.region.region).returned);
    }
    return result;
}

BatchProgram ServingStructure::pipeline_result(const ServingAction & action, std::vector<RankWork> & work) {
    const auto & batch = action.work->batch;
    const auto & flow = flows_.at(action.flow);
    const auto tp = topology_.tensor_parallel_size();
    const auto stages = topology_.stage_count();
    BatchProgram result{ std::nullopt, {}, {} };
    for (std::size_t tensor_rank = 0; tensor_rank < tp; ++tensor_rank) {
        const auto last = (stages - 1) * tp + tensor_rank;
        std::map<std::string, std::size_t> payload{
            { "next_token_ids", flow.outputs[last].at("sampled_tokens") }
        };
        const auto send_begin = graph_.operations.size();
        const auto sent = ranks_[last]->message("pipeline_send_output", batch, payload, true);
        ranks_[last]->wait_message("output", batch, sent);
        work.push_back({ last, send_begin, graph_.operations.size() });

        // Native PP circulates sampled tokens through every stage before its local result is consumed.
        for (std::size_t stage = 0; stage < stages; ++stage) {
            const auto rank = stage * tp + tensor_rank;
            const auto begin = graph_.operations.size();
            const auto received = ranks_[rank]->message("pipeline_receive_output", batch, payload, false);
            payload = received.region.ports.outputs;
            const auto tokens = ranks_[rank]->receive_result(batch, payload.at("next_token_ids"));
            auto program = ranks_[rank]->complete(batch, action.work->effects, action.lane, flow.outputs[rank], tokens);
            if (program.publication) {
                if (result.publication) throw std::invalid_argument("one serving action has multiple publishing roles");
                result.publication = program.publication;
            }
            if (rank == 0) result.returned_pages = std::move(program.returned_pages);
            result.completions.push_back(program.completion);
            if (stage + 1 < stages) {
                const auto forwarded = ranks_[rank]->message("pipeline_send_output", batch, payload, true);
                ranks_[rank]->wait_message("output", batch, forwarded);
            }
            work.push_back({ rank, begin, graph_.operations.size() });
        }
    }
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
    for (auto & rank : ranks_) {
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
    std::vector<std::size_t *> roots;
    std::set<std::size_t> storage;
    for (auto & rank : ranks_) rank->live_state(roots, storage);
    for (auto & [_, flow] : flows_) {
        for (auto & inputs : flow.inputs) {
            for (auto & [name, value] : inputs) roots.push_back(&value);
        }
        for (auto & output : flow.outputs) {
            for (auto & [name, value] : output) roots.push_back(&value);
        }
    }
    return graph_.retire(roots, std::move(storage));
}

nlohmann::json ServingStructure::summary() const {
    const auto & environment = assets_.front()->environment();
    const auto & implementation = environment.at("implementation");
    nlohmann::json result{
        {         "framework",environment.at("framework")                              },
        {             "model",        implementation.at("model_path") },
        {             "dtype",             implementation.at("dtype") },
        { "attention_backend", implementation.at("attention_backend") },
        {        "logical_tp",       topology_.tensor_parallel_size() },
        {        "logical_pp",                topology_.stage_count() },
        {        "logical_ep",           environment.at("logical_ep") },
        {          "topology",        topology_.parallel().document() },
        {            "counts",
         { { "batches", batch_count_ },
         { "actions", action_count_ },
         { "control_decisions", control_count_ },
         { "operations", operation_count_ },
         { "execution_nodes", node_count_ },
         { "execution_dependencies", dependency_count_ } }           },
        {      "rank_sources",                nlohmann::json::array() }
    };
    for (const auto & rank : ranks_) result["rank_sources"].push_back(rank->source_summary());

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
