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
    const bool local = action.step == ServingStep::Prepare || action.step == ServingStep::Compute || action.step == ServingStep::Send
                       || action.step == ServingStep::Wait || (action.step == ServingStep::Receive && !action.channel.empty())
                       || ((action.step == ServingStep::ReadResult || action.step == ServingStep::Result) && action.stage);
    if (local) {
        for (const auto rank : group.members) {
            auto program = rank_step(action, rank, work);
            if (program.publication) {
                if (result.publication) throw std::invalid_argument("one serving action has multiple publishing roles");
                result.publication = program.publication;
            }
            if (rank == group.members.front()) result.returned_pages = std::move(program.returned_pages);
            result.completions.insert(result.completions.end(), program.completions.begin(), program.completions.end());
            result.values.merge(program.values);
        }
    }
    else if (action.step == ServingStep::Stage) result = pipeline_stage(action, work);
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
        graph_.regions[region]["task"] = {
            {    "group",    action.task.group },
            { "sequence", action.task.sequence }
        };
        graph_.regions[region]["lane"] = action.lane;
    }
    ++action_count_;
    if (action.step == ServingStep::Batch || action.step == ServingStep::Model
        || ((action.step == ServingStep::Stage || action.step == ServingStep::Compute) && action.stage == 0))
        ++batch_count_;
    operation_count_ += graph_.operations.size() - operation_begin;
    node_count_ += graph_.execution.nodes().size() - node_begin;
    dependency_count_ += graph_.execution.dependencies().size() - dependency_begin;

    return result;
}

BatchProgram ServingStructure::rank_step(const ServingAction & action, std::size_t rank, std::vector<RankWork> & work, bool handoff) {
    const auto begin = graph_.operations.size();
    const auto & batch = action.work->batch;
    auto & builder = *ranks_.at(rank);
    if (action.step == ServingStep::Prepare) flows_.try_emplace(action.flow);
    auto & flow = flows_.at(action.flow);
    BatchProgram result{ std::nullopt, {}, {} };
    switch (action.step) {
    case ServingStep::Prepare: {
        auto prepared = builder.prepare(batch, action.lane);
        flow.inputs[rank] = std::move(prepared.ports.outputs);
        result.completions.push_back(graph_.execution.region_call(prepared.region).returned);
        break;
    }
    case ServingStep::Compute: {
        auto model = builder.model(batch, flow.inputs.at(rank));
        flow.inputs.erase(rank);
        flow.outputs[rank] = std::move(model.ports.outputs);
        flow.outputs[rank].erase("token_relay");
        auto values = nlohmann::json::object();
        for (const auto & [name, value] : flow.outputs.at(rank)) values[name] = graph_.types.at(value);
        result.values.emplace(std::to_string(rank), std::move(values));
        result.completions.push_back(graph_.execution.region_call(model.region).returned);
        break;
    }
    case ServingStep::Send: {
        auto & payload = flow.payloads[rank][action.channel];
        if (action.channel == "proxy") {

            // A stage sends its computed proxy, not the proxy received from its predecessor.
            payload.clear();
            auto & output = flow.outputs.at(rank);
            for (auto item = output.begin(); item != output.end();) {
                if (item->first.starts_with("proxy.")) {
                    payload.emplace(item->first.substr(6), item->second);
                    item = output.erase(item);
                }
                else ++item;
            }
        }
        else if (payload.empty()) payload.emplace("next_token_ids", flow.outputs.at(rank).at("sampled_tokens"));
        auto message = builder.message("pipeline_send_" + action.channel, batch, payload, true);
        result.completions.push_back(graph_.execution.region_call(message.region.region).returned);
        flow.messages[rank].emplace(action.channel, std::move(message));
        break;
    }
    case ServingStep::Receive: {
        const auto & members = topology_.parallel().group(rank, "pp").members;
        const auto position = std::ranges::find(members, rank) - members.begin();
        const auto sender = members[(static_cast<std::size_t>(position) + members.size() - 1) % members.size()];

        // Native receive first obtains the sender's metadata; no fabricated model-specific dimensions are needed.
        auto message = builder.message("pipeline_receive_" + action.channel, batch, flow.payloads.at(sender).at(action.channel), false);
        flow.payloads[rank][action.channel] = std::move(message.region.ports.outputs);
        if (action.channel == "proxy") {
            for (const auto & [name, value] : flow.payloads.at(rank).at(action.channel)) flow.inputs.at(rank).emplace("proxy." + name, value);
        }
        result.completions.push_back(graph_.execution.region_call(message.region.region).returned);
        break;
    }
    case ServingStep::Wait: {
        auto & messages = flow.messages.at(rank);
        result.completions = builder.wait_message(action.channel, batch, messages.at(action.channel));
        messages.erase(action.channel);
        if (messages.empty()) flow.messages.erase(rank);
        break;
    }
    case ServingStep::ReadResult: {
        const auto tokens = topology_.stage_count(instance_.id) == 1 ? flow.outputs.at(rank).at("sampled_tokens")
                                                                     : builder.receive_result(batch, flow.payloads.at(rank).at("output").at("next_token_ids"));
        flow.tokens[rank] = tokens;
        result.completions.push_back(builder.begin_result(batch, tokens).value());
        break;
    }
    case ServingStep::Result: {
        const auto program = builder.complete(batch,
                                              action.work->effects,
                                              action.lane,
                                              flow.outputs.at(rank),
                                              flow.tokens.at(rank),
                                              action.work->metadata_slots,
                                              handoff || instance_.id == "prefill");
        result.publication = program.publication;
        result.returned_pages = program.returned_pages;
        result.completions.push_back(program.completion);
        flow.outputs.erase(rank);
        flow.tokens.erase(rank);
        break;
    }
    default:
        throw std::logic_error("unknown local serving step");
    }
    work.push_back({ rank, begin, graph_.operations.size() });
    if (flow.inputs.empty() && flow.outputs.empty() && flow.messages.empty()) flows_.erase(action.flow);
    return result;
}

BatchProgram ServingStructure::pipeline_stage(const ServingAction & action, std::vector<RankWork> & work) {
    auto step = action;
    const auto stage = action.stage.value();
    if (stage == 0) {
        step.step = ServingStep::Prepare;
        for (const auto rank : instance_.members) rank_step(step, rank, work);
    }
    BatchProgram result{ std::nullopt, {}, {} };
    const auto & members = topology_.stage(instance_.id, stage).members;
    for (std::size_t peer = 0; peer < members.size(); ++peer) {
        const auto rank = members[peer];
        step.step = ServingStep::Compute;
        auto model = rank_step(step, rank, work);
        result.completions.insert(result.completions.end(), model.completions.begin(), model.completions.end());
        result.values.merge(model.values);
        if (stage + 1 == topology_.stage_count(instance_.id)) continue;

        step.channel = "proxy";
        step.step = ServingStep::Send;
        auto sent = rank_step(step, rank, work);
        step.step = ServingStep::Wait;
        rank_step(step, rank, work);
        result.completions.insert(result.completions.end(), sent.completions.begin(), sent.completions.end());
        step.step = ServingStep::Receive;
        auto received = rank_step(step, topology_.stage(instance_.id, stage + 1).members[peer], work);
        result.completions.insert(result.completions.end(), received.completions.begin(), received.completions.end());
    }
    return result;
}

BatchProgram ServingStructure::read_result(const ServingAction & action, std::vector<RankWork> & work) {
    BatchProgram result{ std::nullopt, {}, {} };
    for (const auto rank : instance_.members) {
        auto program = rank_step(action, rank, work);
        result.completions.insert(result.completions.end(), program.completions.begin(), program.completions.end());
    }
    return result;
}

BatchProgram ServingStructure::pipeline_result(const ServingAction & action, std::vector<RankWork> & work) {
    const auto tp = topology_.tensor_parallel_size(instance_.id);
    const auto stages = topology_.stage_count(instance_.id);
    auto step = action;
    BatchProgram result{ std::nullopt, {}, {} };
    std::map<std::size_t, std::size_t> completions;
    for (std::size_t tensor_rank = 0; tensor_rank < tp; ++tensor_rank) {
        const auto last = topology_.stage(instance_.id, stages - 1).members[tensor_rank];
        step.channel = "output";
        if (stages > 1) {
            step.step = ServingStep::Send;
            rank_step(step, last, work);
            step.step = ServingStep::Wait;
            rank_step(step, last, work);
        }
        for (std::size_t stage = 0; stage < stages; ++stage) {
            const auto rank = topology_.stage(instance_.id, stage).members[tensor_rank];
            if (stages > 1) {
                step.step = ServingStep::Receive;
                rank_step(step, rank, work);
            }
            if (!flows_.at(action.flow).tokens.contains(rank)) {
                step.step = ServingStep::ReadResult;
                rank_step(step, rank, work);
            }
            step.step = ServingStep::Result;
            auto program = rank_step(step, rank, work, action.step == ServingStep::Model);
            if (program.publication) {
                if (result.publication) throw std::invalid_argument("one serving action has multiple publishing roles");
                result.publication = program.publication;
            }
            if (rank == instance_.members.front()) result.returned_pages = std::move(program.returned_pages);
            completions.emplace(rank, program.completions.front());
            if (stage + 1 < stages) {
                step.step = ServingStep::Send;
                rank_step(step, rank, work);
                step.step = ServingStep::Wait;
                rank_step(step, rank, work);
            }
        }
    }
    for (const auto rank : instance_.members) result.completions.push_back(completions.at(rank));
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
        for (auto & [rank, channels] : flow.payloads) {
            for (auto & [channel, payload] : channels) {
                for (auto & [name, value] : payload) roots.values.push_back(&value);
            }
        }
        for (auto & [rank, messages] : flow.messages) {
            for (auto & [channel, message] : messages) {
                roots.regions.push_back(&message.region.region);
                for (auto & [name, value] : message.region.ports.outputs) roots.values.push_back(&value);
            }
        }
        for (auto & [rank, tokens] : flow.tokens) roots.values.push_back(&tokens);
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
