#include "markov/adapters/frameworks/sglang/structure.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace markov::sglang {

ServingStructure::ServingStructure(std::span<const TemplateAsset> assets, ServingSettings settings, std::span<const Request> requests,
                                   const ParallelTopology & topology)
    : topology_(topology),
      parallel_(graph_),
      prefill_token_granularity_(settings.page_size),
      assets_(assets.size()) {
    if (assets.empty()) throw std::invalid_argument("serving requires rank assets");
    if (topology_.ranks().size() != assets.size()) throw std::invalid_argument("rank placement does not match its source bindings");
    const auto addressable_pages = (static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + 1) / settings.page_size;
    if (settings.max_total_tokens / settings.page_size >= addressable_pages)
        throw std::invalid_argument("target KV indices exceed the native int32 range including reserved page zero");

    for (const auto & asset : assets) {
        const auto & environment = asset.environment();
        if (environment.at("framework") != "sglang" || environment.at("logical_tp").get<std::size_t>() != assets.size())
            throw std::invalid_argument("SGLang serving needs one source for every logical TP rank");
        if (environment.at("workspace_policy").at("kind") != "native_attention_bound_reservation")
            throw std::invalid_argument("SGLang serving requires its declared native workspace warmup");
        const auto rank = environment.at("rank").get<std::size_t>();
        if (rank >= assets_.size() || assets_[rank] != nullptr) throw std::invalid_argument("rank assets are missing, duplicated or outside the TP group");
        for (const auto * key : { "implementation", "torch", "torch_npu", "cann", "device", "source_transport" }) {
            if (environment.at(key) != assets.front().environment().at(key)) throw std::invalid_argument("rank sources have different implementation domains");
        }
        assets_[rank] = &asset;
    }

    // Follow TpModelWorker.get_worker_info and Scheduler.init_req_max_new_tokens; fixed outputs must not be clamped.
    const auto context = assets.front().environment().at("implementation").at("model_config").at("context_len").get<std::int64_t>();
    const auto capacity = settings.max_total_tokens / settings.page_size * settings.page_size;
    const auto request_limit = std::min(context, capacity) - 1;
    for (const auto & request : requests) {
        if (request.prompt_tokens > 0 && request.output_tokens > 0) {
            if (request.prompt_tokens >= request_limit - 5) throw std::invalid_argument("request exceeds the native input length limit: " + request.id);

            const auto paged_input = ((request.prompt_tokens - 1) / settings.page_size + 1) * settings.page_size;
            const auto output_limit = std::min(request_limit - request.prompt_tokens - 1, capacity - paged_input - settings.page_size - 1);
            if (request.output_tokens > output_limit) throw std::invalid_argument("request output would be truncated by native serving: " + request.id);
        }
    }

    for (std::size_t rank = 0; rank < assets_.size(); ++rank) {
        const auto offset = graph_.execution_resources.size();
        const auto & asset = *assets_[rank];
        const auto & resources = asset.execution_resources();
        graph_.execution_resources.insert(graph_.execution_resources.end(), resources.begin(), resources.end());
        ranks_.push_back(std::make_unique<RankStructure>(asset, settings, graph_, offset, rank));
    }
}

GroupAdmission ServingStructure::admission() const {

    // PrefillAdder._update_prefill_budget charges whole pages on the fixed-output, cold-cache path.
    GroupAdmission result;
    for (const auto & group : topology_.groups()) {
        result.emplace(group.id, AdmissionConstraints{ ranks_[group.members.front()]->admission_members(), prefill_token_granularity_ });
    }
    return result;
}

BatchProgram ServingStructure::expand(const ServingAction & action) {
    const auto & group = topology_.group(action.task.group);
    const auto operation_begin = graph_.operations.size();
    const auto node_begin = graph_.execution.nodes().size();
    const auto dependency_begin = graph_.execution.dependencies().size();
    const auto region_begin = graph_.regions.size();
    std::vector<RankWork> work;
    std::vector<RankProgram> programs;

    for (const auto rank : group.members) {
        const auto begin = graph_.operations.size();
        programs.push_back(ranks_[rank]->expand(action.batch, action.effects));
        work.push_back({ rank, begin, graph_.operations.size() });
    }
    parallel_.connect(work, topology_);

    for (std::size_t region = region_begin; region < graph_.regions.size(); ++region) graph_.regions[region]["batch"] = batch_count_;
    ++batch_count_;
    operation_count_ += graph_.operations.size() - operation_begin;
    node_count_ += graph_.execution.nodes().size() - node_begin;
    dependency_count_ += graph_.execution.dependencies().size() - dependency_begin;

    // Publication follows observed role ports; local completion remains necessary at every participating rank.
    BatchProgram result{ std::nullopt, {}, std::move(programs.front().returned_pages) };
    for (const auto & program : programs) {
        if (program.publication) {
            if (result.publication) throw std::invalid_argument("one serving action has multiple publishing roles");
            result.publication = program.publication;
        }
        result.completions.push_back(program.completion);
    }
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
        {        "logical_tp",                         assets_.size() },
        {          "topology",                   topology_.document() },
        {            "counts",
         { { "batches", batch_count_ },
         { "control_decisions", control_count_ },
         { "operations", operation_count_ },
         { "execution_nodes", node_count_ },
         { "execution_dependencies", dependency_count_ } }           },
        {      "rank_sources",                nlohmann::json::array() }
    };
    for (std::size_t rank = 0; rank < assets_.size(); ++rank)
        result["rank_sources"].push_back({
            {                                 "rank",                                                                                 rank },
            {                    "physical_coverage",                                      assets_[rank]->physical_coverage().at("status") },
            {                     "runtime_coverage",                                       assets_[rank]->runtime_coverage().at("status") },
            { "workspace_capacity_lower_bound_bytes", assets_[rank]->environment().at("workspace_policy").at("capacity_lower_bound_bytes") }
        });

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
    return result;
}

nlohmann::json ServingStructure::document() const {
    auto result = graph_.document();
    result["scope"] = "serving_structure";
    result["environment"] = assets_.front()->environment();
    result["environment"].erase("rank");
    result["topology"] = topology_.document();
    return result;
}

} // namespace markov::sglang
