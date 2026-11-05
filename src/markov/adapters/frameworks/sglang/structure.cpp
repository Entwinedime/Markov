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

    const bool experts = std::ranges::any_of(assets_, [](const auto * asset) { return !asset->entrypoints("expert_layer").empty(); });
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
    const bool local =
        action.step == ServingStep::Schedule || action.step == ServingStep::PrepareInputs || action.step == ServingStep::Allocate
        || action.step == ServingStep::PrepareOutputs || action.step == ServingStep::Prepare || action.step == ServingStep::Compute
        || (action.step == ServingStep::Send && !action.channel.empty()) || action.step == ServingStep::Wait
        || (action.step == ServingStep::Receive && !action.channel.empty())
        || ((action.step == ServingStep::PrepareResult || action.step == ServingStep::ReadResult || action.step == ServingStep::Result) && action.stage);
    const bool whole_pipeline = (action.step == ServingStep::Batch || action.step == ServingStep::Model) && topology_.stage_count(instance_.id) > 1;
    const bool composite =
        !local && (action.step == ServingStep::Stage || action.step == ServingStep::ReadResult || action.step == ServingStep::Result || whole_pipeline);
    if (!composite) {
        for (const auto rank : group.members) {
            const auto begin = graph_.operations.size();
            const auto rank_node_begin = graph_.execution.first_node() + graph_.execution.nodes().size();
            const auto rank_dependency_begin = graph_.execution.dependencies().size();
            BatchProgram program{ std::nullopt, {}, {} };
            if (local) program = rank_step(action, rank, work);
            else {
                auto single = ranks_.at(rank)->expand(action);
                program = { single.publication, { single.completion }, std::move(single.returned_pages) };
                work.push_back({ rank, begin, graph_.operations.size() });
            }
            if (const auto required = prerequisites.find(rank); required != prerequisites.end()) {
                const auto consumers = graph_.execution.initial_nodes(rank_node_begin, rank_dependency_begin);
                for (const auto producer : required->second) {
                    for (const auto consumer : consumers) graph_.execution.depend(producer, consumer, "instance_input_ready");
                }
            }
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
    else if (whole_pipeline) {

        // Simple PD control submits a whole batch; its execution still uses the shared stage and message expansion.
        auto staged = action;
        for (std::size_t stage = 0; stage < topology_.stage_count(instance_.id); ++stage) {
            staged.stage = stage;
            pipeline_stage(staged, work);
        }
        staged.stage.reset();
        result = pipeline_result(staged, work);
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
    if ((action.step == ServingStep::Batch || action.step == ServingStep::Model
         || ((action.step == ServingStep::Stage || action.step == ServingStep::Compute) && action.stage == 0))
        && group.members.front() == topology_.parallel().group(group.members.front(), "tp").members.front())
        ++batch_count_;
    operation_count_ += graph_.operations.size() - operation_begin;
    node_count_ += graph_.execution.nodes().size() - node_begin;
    dependency_count_ += graph_.execution.dependencies().size() - dependency_begin;

    return result;
}

BatchProgram ServingStructure::rank_step(const ServingAction & action, std::size_t rank, std::vector<RankWork> & work, bool retain_unfinished) {
    const auto begin = graph_.operations.size();
    auto & builder = *ranks_.at(rank);
    if (action.step == ServingStep::PrepareInputs || action.step == ServingStep::Allocate || action.step == ServingStep::PrepareOutputs) {
        const auto & batch = action.batch_work().batch;
        const auto program = action.step == ServingStep::PrepareInputs ? builder.prepare_inputs(batch, action.lane)
                             : action.step == ServingStep::Allocate    ? builder.allocate(batch)
                                                                       : builder.prepare_outputs(batch, action.lane);
        work.push_back({ rank, begin, graph_.operations.size() });
        return { std::nullopt, { program.completion }, {} };
    }
    if (action.step == ServingStep::Schedule) {
        const std::map<std::string, std::size_t> empty;
        const auto * outputs = &empty;
        if (const auto flow = flows_.find(action.flow); flow != flows_.end()) {
            if (const auto view = flow->second.find(rank); view != flow->second.end() && view->second.inputs) outputs = &*view->second.inputs;
        }
        const auto program = builder.schedule(action.schedule_operation(), *outputs);
        work.push_back({ rank, begin, graph_.operations.size() });
        return { std::nullopt, { program.completion }, {} };
    }

    const auto & batch = action.batch_work().batch;
    if (action.step == ServingStep::Prepare) flows_[action.flow].try_emplace(rank);
    auto & flow = flows_.at(action.flow);
    auto & view = flow.at(rank);
    BatchProgram result{ std::nullopt, {}, {} };
    switch (action.step) {
    case ServingStep::Prepare: {
        auto prepared = builder.prepare(action.batch_work(), action.lane);
        view.inputs = std::move(prepared.ports.outputs);
        result.completions.push_back(graph_.execution.region_call(prepared.region).returned);
        break;
    }
    case ServingStep::Compute: {
        auto model = builder.model(batch, view.inputs.value());
        view.inputs.reset();
        view.outputs = std::move(model.ports.outputs);
        view.outputs->erase("token_relay");
        auto values = nlohmann::json::object();
        for (const auto & [name, value] : *view.outputs) values[name] = graph_.types.at(value);
        result.values.emplace(std::to_string(rank), std::move(values));
        result.completions.push_back(graph_.execution.region_call(model.region).returned);
        break;
    }
    case ServingStep::Send: {
        auto & payload = view.payloads[action.channel];
        if (action.channel == "proxy") {

            // A stage sends its computed proxy, not the proxy received from its predecessor.
            payload.clear();
            auto & output = view.outputs.value();
            for (auto item = output.begin(); item != output.end();) {
                if (item->first.starts_with("proxy.")) {
                    payload.emplace(item->first.substr(6), item->second);
                    item = output.erase(item);
                }
                else ++item;
            }
        }
        else if (payload.empty()) payload.emplace("next_token_ids", view.outputs.value().at("sampled_tokens"));
        auto message = builder.message("pipeline_send_" + action.channel, batch, payload, true);
        result.completions.push_back(graph_.execution.region_call(message.region.region).returned);
        view.messages.emplace(action.channel, std::move(message));
        break;
    }
    case ServingStep::Receive: {
        const auto & members = topology_.parallel().group(rank, "pp").members;
        const auto position = std::ranges::find(members, rank) - members.begin();
        const auto sender = members[(static_cast<std::size_t>(position) + members.size() - 1) % members.size()];

        // Native receive first obtains the sender's metadata; no fabricated model-specific dimensions are needed.
        auto message = builder.message("pipeline_receive_" + action.channel, batch, flow.at(sender).payloads.at(action.channel), false);
        view.payloads[action.channel] = std::move(message.region.ports.outputs);
        if (action.channel == "proxy") {
            for (const auto & [name, value] : view.payloads.at(action.channel)) view.inputs.value().emplace("proxy." + name, value);
        }
        result.completions.push_back(graph_.execution.region_call(message.region.region).returned);
        break;
    }
    case ServingStep::Wait: {
        result.completions = builder.wait_message(action.channel, batch, view.messages.at(action.channel));
        view.messages.erase(action.channel);
        break;
    }
    case ServingStep::PrepareResult: {
        const auto tokens = view.payloads.at("output").at("next_token_ids");

        // Preparation updates the shared token relay; the received output itself is unchanged.
        view.tokens = tokens;
        result.completions.push_back(builder.prepare_result(batch, tokens, view.outputs.value().at("request_slots")));
        break;
    }
    case ServingStep::ReadResult: {
        if (topology_.stage_count(instance_.id) == 1) {
            const auto tokens = view.outputs.value().at("sampled_tokens");
            view.tokens = tokens;
            result.completions.push_back(builder.begin_result(batch, tokens).value());
        }
        else result.completions.push_back(builder.read_pipeline_result(batch, view.tokens.value()));
        break;
    }
    case ServingStep::Result: {
        const auto program = builder.complete(batch,
                                              action.batch_work().effects,
                                              action.lane,
                                              view.outputs.value(),
                                              view.tokens.value(),
                                              action.batch_work().metadata_slots,
                                              retain_unfinished,
                                              action.batch_work().index_tail);
        result.publication = program.publication;
        result.returned_pages = program.returned_pages;
        result.completions.push_back(program.completion);
        view.outputs.reset();
        view.tokens.reset();
        break;
    }
    default:
        throw std::logic_error("unknown local serving step");
    }
    work.push_back({ rank, begin, graph_.operations.size() });

    // The output ring may forward a finished rank's payload until every rank has released its work.
    if (std::ranges::all_of(flow, [](const auto & entry) {
            const auto & state = entry.second;
            return !state.inputs && !state.outputs && state.messages.empty();
        }))
        flows_.erase(action.flow);
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
                step.step = ServingStep::PrepareResult;
                rank_step(step, rank, work);
            }
            if (stages > 1 || !flows_.at(action.flow).at(rank).tokens) {
                step.step = ServingStep::ReadResult;
                rank_step(step, rank, work);
            }
            step.step = ServingStep::Result;
            auto program = rank_step(step, rank, work, true);
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

std::vector<std::size_t> ServingStructure::retire() {
    GraphRoots roots;
    live_state(roots);
    return graph_.retire(std::move(roots));
}

void ServingStructure::live_state(GraphRoots & roots) {
    parallel_.live_state(roots);
    for (auto & [_, rank] : ranks_) rank->live_state(roots);
    for (auto & [_, flow] : flows_) {
        for (auto & [rank, view] : flow) {
            if (view.inputs) {
                for (auto & [name, value] : *view.inputs) roots.values.push_back(&value);
            }
            if (view.outputs) {
                for (auto & [name, value] : *view.outputs) roots.values.push_back(&value);
            }

            for (auto & [channel, payload] : view.payloads) {
                for (auto & [name, value] : payload) roots.values.push_back(&value);
            }

            for (auto & [channel, message] : view.messages) {
                roots.regions.push_back(&message.region.region);
                for (auto & [name, value] : message.region.ports.outputs) roots.values.push_back(&value);
            }

            if (view.tokens) roots.values.push_back(&*view.tokens);
        }
    }
}

PageReuse ServingStructure::page_reuse() const { return ranks_.begin()->second->page_reuse(); }
std::span<const TemplateAsset * const> ServingStructure::sources() const { return assets_; }
TargetRegion ServingStructure::transfer(std::size_t rank, std::int64_t bytes, std::int64_t segments) { return ranks_.at(rank)->transfer(bytes, segments); }

std::size_t ServingStructure::transport_event(std::size_t rank, const std::string & role, std::int64_t prompt_tokens) {
    return ranks_.at(rank)->transport_event(role, prompt_tokens);
}

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
