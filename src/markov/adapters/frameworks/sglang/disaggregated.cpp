#include "markov/adapters/frameworks/sglang/disaggregated.hpp"

#include <algorithm>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace markov::sglang {
namespace {

ParallelTopology topology(const DisaggregatedGroups & groups, std::span<const std::int64_t> devices) {
    if (!devices.empty() && (devices.size() != 2 || devices[0] < 0 || devices[1] < 0 || devices[0] == devices[1]))
        throw std::invalid_argument("PD placement requires two distinct local devices");
    const auto p = devices.empty() ? 0 : static_cast<std::size_t>(devices[0]);
    const auto d = devices.empty() ? 1 : static_cast<std::size_t>(devices[1]);
    return {
        { { "local", p }, { "local", d } },
        { { groups.prefill, "serving", { 0 } },
         { groups.decode, "serving", { 1 } },
         { groups.transfer, "kv", { 0, 1 } },
         { "prefill.tp", "tp", { 0 } },
         { "decode.tp", "tp", { 1 } },
         { "prefill.attention_tp", "attention_tp", { 0 } },
         { "decode.attention_tp", "attention_tp", { 1 } } }
    };
}

} // namespace

DisaggregatedStructure::DisaggregatedStructure(std::span<const TemplateAsset> assets, ServingSettings prefill, ServingSettings decode,
                                               std::span<const Request> requests, const DisaggregatedGroups & groups, std::span<const std::int64_t> devices)
    : topology_(topology(groups, devices)),
      parallel_(graph_),
      assets_(2) {
    if (assets.size() != 2) throw std::invalid_argument("PD requires one prefill and one decode source");
    for (const auto & asset : assets) {
        const auto & environment = asset.environment();
        const auto & mode = environment.at("implementation").at("disaggregation_mode");
        if ((mode != "prefill" && mode != "decode") || environment.at("logical_tp") != 1 || environment.at("logical_pp") != 1
            || environment.at("source_transport") != "native_disaggregation")
            throw std::invalid_argument("PD requires native TP1/PP1 disaggregated roles");
        const std::size_t rank = mode == "prefill" ? 0 : 1;
        if (assets_[rank] || environment.at("rank") != rank) throw std::invalid_argument("PD role identities disagree");
        assets_[rank] = &asset;
    }
    auto implementation = assets_[0]->environment().at("implementation");
    auto other = assets_[1]->environment().at("implementation");
    implementation.erase("disaggregation_mode");
    other.erase("disaggregation_mode");
    if (implementation != other) throw std::invalid_argument("PD instances require the same model and implementation");
    for (const auto * field : { "torch", "torch_npu", "cann", "device" }) {
        if (assets_[0]->environment().at(field) != assets_[1]->environment().at(field)) throw std::invalid_argument("PD runtime domains differ");
    }
    if (assets_[0]->environment().at("kv_layout").at("page_size") != prefill.page_size || prefill.page_size != decode.page_size)
        throw std::invalid_argument("PD transfer requires the source KV page layout");
    RankStructure::check_target(*assets_[0], prefill, requests, false);
    RankStructure::check_target(*assets_[1], decode, requests, true);

    for (std::size_t rank = 0; rank < assets_.size(); ++rank) {
        if (assets_[rank]->environment().at("workspace_policy").at("kind") != "native_attention_bound_reservation")
            throw std::invalid_argument("PD requires its declared native workspace warmup");
        const auto offset = graph_.execution_resources.size();
        const auto & resources = assets_[rank]->execution_resources();
        graph_.execution_resources.insert(graph_.execution_resources.end(), resources.begin(), resources.end());

        // Simulation starts after uniform warmup and drain; event signals still require actual producers.
        for (std::size_t resource = 0; resource < resources.size(); ++resource) {
            if (resources[resource] == "stream" || resources[resource] == "thread") graph_.execution.initially_idle(offset + resource);
        }
        ranks_.push_back(std::make_unique<RankStructure>(*assets_[rank], rank == 0 ? prefill : decode, graph_, offset, rank, 1));
    }
    if (ranks_[0]->page_reuse() != ranks_[1]->page_reuse()) throw std::invalid_argument("PD instances require matching page reuse policies");
}

GroupAdmission DisaggregatedStructure::admission() const { return {}; }
PageReuse DisaggregatedStructure::page_reuse() const { return ranks_[0]->page_reuse(); }

void DisaggregatedStructure::require(std::span<const std::size_t> producers, std::size_t begin, const std::string & kind) {
    const auto consumers = graph_.execution.initial_nodes(begin);
    for (const auto producer : producers) {
        for (const auto consumer : consumers) graph_.execution.depend(producer, consumer, kind);
    }
}

std::vector<std::size_t> DisaggregatedStructure::transfer(const ServingAction & action, const Handoff & handoff) {
    const auto & layout = assets_[0]->environment().at("kv_layout");
    const auto buffers = layout.at("buffer_item_bytes").get<std::vector<std::int64_t>>();
    const auto metadata = layout.at("metadata_item_bytes").get<std::vector<std::int64_t>>();
    const auto & pages = action.work->transferred_pages;
    std::int64_t runs = 0;
    for (std::size_t page = 0; page < pages.size(); ++page) {
        if (page == 0 || pages[page].source != pages[page - 1].source + 1 || pages[page].destination != pages[page - 1].destination + 1) ++runs;
    }
    const auto bytes = std::accumulate(buffers.begin(), buffers.end(), std::int64_t{ 0 }) * static_cast<std::int64_t>(pages.size());
    const auto begin = graph_.execution.first_node() + graph_.execution.nodes().size();
    const auto kv = ranks_[0]->transfer(bytes, runs * static_cast<std::int64_t>(buffers.size()));
    require(handoff.reserved, begin, "destination_reserved");
    require(handoff.prepared, begin, "source_kv_ready");

    // The native protocol publishes metadata only after all KV segments have completed.
    const auto kv_done = graph_.execution.region_call(kv.region).returned;
    const auto auxiliary_bytes = std::accumulate(metadata.begin(), metadata.end(), std::int64_t{ 0 });
    const auto auxiliary = ranks_[0]->transfer(auxiliary_bytes, static_cast<std::int64_t>(metadata.size()));
    graph_.execution.depend(kv_done, graph_.execution.region_call(auxiliary.region).entry, "kv_before_metadata");
    kv_bytes_ += bytes;
    metadata_bytes_ += auxiliary_bytes;
    return { graph_.execution.region_call(auxiliary.region).returned };
}

BatchProgram DisaggregatedStructure::expand(const ServingAction & action) {
    const auto operation_begin = graph_.operations.size();
    const auto node_begin = graph_.execution.first_node() + graph_.execution.nodes().size();
    const auto dependency_begin = graph_.execution.dependencies().size();
    const auto region_begin = graph_.regions.size();
    auto & handoff = handoffs_[action.flow];
    const auto & batch = action.work->batch;
    const auto & effects = action.work->effects;
    std::size_t rank = 1;
    RankProgram local{ std::nullopt, 0, {} };
    switch (action.step) {
    case ServingStep::Reserve:
        local = ranks_[1]->reserve(batch);
        break;
    case ServingStep::Model:
        rank = 0;
        local = ranks_[0]->prefill_handoff(batch, effects, action.flow);
        ++batches_;
        break;
    case ServingStep::Transfer:
        rank = 0;
        handoff.transferred = transfer(action, handoff);
        break;
    case ServingStep::Accept:
        local = ranks_[1]->accept(batch, effects, action.flow);
        require(handoff.transferred, node_begin, "destination_kv_ready");
        handoff.accepted = true;
        break;
    case ServingStep::Release:
        rank = 0;
        local = ranks_[0]->release(effects);
        require(handoff.transferred, node_begin, "source_reusable");
        handoff.released = true;
        break;
    case ServingStep::Batch:
        local = ranks_[1]->expand(batch, effects);
        ++batches_;
        break;
    default:
        throw std::invalid_argument("PD expansion cannot execute a pipeline stage");
    }
    const RankWork work{ rank, operation_begin, graph_.operations.size() };
    parallel_.connect(std::span(&work, 1), topology_, action.task);
    auto completions = graph_.execution.terminal_nodes(node_begin);
    if (action.step == ServingStep::Reserve) handoff.reserved = completions;
    if (action.step == ServingStep::Model) handoff.prepared = completions;
    if (handoff.accepted && handoff.released) handoffs_.erase(action.flow);
    else if (action.step == ServingStep::Batch) handoffs_.erase(action.flow);

    for (auto region = region_begin; region < graph_.regions.size(); ++region) {
        graph_.regions[region]["batch"] = action.flow;
        graph_.regions[region]["action"] = action.task.sequence;
    }
    ++actions_;
    count(operation_begin, node_begin, dependency_begin);
    return { local.publication, std::move(completions), std::move(local.returned_pages) };
}

void DisaggregatedStructure::count(std::size_t operation_begin, std::size_t node_begin, std::size_t dependency_begin) {
    operations_ += graph_.operations.size() - operation_begin;
    nodes_ += graph_.execution.first_node() + graph_.execution.nodes().size() - node_begin;
    dependencies_ += graph_.execution.dependencies().size() - dependency_begin;
}

std::vector<std::size_t> DisaggregatedStructure::idle() {
    const auto operation_begin = graph_.operations.size();
    const auto node_begin = graph_.execution.first_node() + graph_.execution.nodes().size();
    const auto dependency_begin = graph_.execution.dependencies().size();
    const auto completion = ranks_[1]->idle();
    count(operation_begin, node_begin, dependency_begin);
    return completion ? graph_.execution.terminal_nodes(node_begin) : std::vector<std::size_t>{};
}

const RegionGraph & DisaggregatedStructure::graph() const { return graph_; }

std::vector<std::size_t> DisaggregatedStructure::retire() {
    std::vector<std::size_t *> values;
    std::set<std::size_t> storage;
    for (auto & rank : ranks_) rank->live_state(values, storage);
    auto nodes = graph_.retire(values, std::move(storage));

    // A source may finish before its peer is admitted; retain those readiness times across IR compaction.
    for (const auto & [_, handoff] : handoffs_) {
        for (const auto * ports : { &handoff.reserved, &handoff.prepared, &handoff.transferred }) nodes.insert(nodes.end(), ports->begin(), ports->end());
    }
    return nodes;
}

nlohmann::json DisaggregatedStructure::summary() const {
    auto sources = nlohmann::json::array();
    for (const auto & rank : ranks_) sources.push_back(rank->source_summary());
    return {
        {     "framework",                              "sglang"                          },
        {          "mode",                                                 "disaggregated" },
        {    "logical_tp",                                                               1 },
        {    "logical_pp",                                                               1 },
        {      "topology",                                            topology_.document() },
        {         "model", assets_[0]->environment().at("implementation").at("model_path") },
        {  "rank_sources",                                              std::move(sources) },
        {        "counts",
         { { "batches", batches_ },
         { "actions", actions_ },
         { "operations", operations_ },
         { "execution_nodes", nodes_ },
         { "execution_dependencies", dependencies_ },
         { "kv_bytes", kv_bytes_ },
         { "metadata_bytes", metadata_bytes_ } }                                          },
        { "qualification",
         { { "scheduling", "single_prefill_and_prebuilt_admission" },
         { "initial_state", "prewarmed_drained" },
         { "transfer", "native_synchronous_sdma_interface" },
         { "token_values", "symbolic" },
         { "costs", "given_execution_kind_ns" } }                                         }
    };
}

nlohmann::json DisaggregatedStructure::document() const {
    auto result = graph_.document();
    result["summary"] = summary();
    return result;
}

} // namespace markov::sglang
