#include "markov/adapters/frameworks/sglang/disaggregated.hpp"

#include <algorithm>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace markov::sglang {

DisaggregatedStructure::DisaggregatedStructure(std::span<const TemplateAsset> assets, ServingSettings prefill, ServingSettings decode,
                                               std::span<const Request> requests, const DisaggregatedGroups & groups, std::span<const std::int64_t> devices,
                                               const nlohmann::json & prefill_routes, const nlohmann::json & decode_routes)
    : topology_(assets, devices),
      groups_(groups),
      prefill_(graph_, assets, prefill, requests, topology_, groups.prefill, prefill_routes),
      decode_(graph_, assets, decode, requests, topology_, groups.decode, decode_routes),
      transfers_(prefill_.sources(), decode_.sources(), prefill.page_size) {
    if (prefill.page_size != decode.page_size) throw std::invalid_argument("PD instances require matching page layouts");
    const auto & first = prefill_.sources().front()->environment();
    const auto & other = decode_.sources().front()->environment();
    for (const auto * field : { "torch", "torch_npu", "cann", "device" }) {
        if (first.at(field) != other.at(field)) throw std::invalid_argument("PD runtime domains differ");
    }
    for (const auto * field : { "model_path", "model_config", "dtype", "attention_backend" }) {
        if (first.at("implementation").at(field) != other.at("implementation").at(field))
            throw std::invalid_argument("PD instances require the same model and implementation");
    }
    if (prefill_.page_reuse() != decode_.page_reuse()) throw std::invalid_argument("PD instances require matching page reuse policies");
}

GroupAdmission DisaggregatedStructure::admission() const {
    auto result = prefill_.admission();
    result.merge(decode_.admission());
    return result;
}

PageReuse DisaggregatedStructure::page_reuse() const { return prefill_.page_reuse(); }

void DisaggregatedStructure::require(std::span<const std::size_t> producers, std::size_t begin, const std::string & kind) {
    const auto consumers = graph_.execution.initial_nodes(begin);
    for (const auto producer : producers) {
        for (const auto consumer : consumers) graph_.execution.depend(producer, consumer, kind);
    }
}

std::size_t DisaggregatedStructure::append_transfer(const KvTransfer & route, std::int64_t bytes, std::int64_t segments,
                                                    std::span<const std::size_t> prerequisites, const std::string & kind, std::span<const KvSlice> slices) {
    const auto begin = graph_.execution.first_node() + graph_.execution.nodes().size();
    const auto copied = prefill_.transfer(route.source, bytes, segments);
    require(prerequisites, begin, kind);
    graph_.regions[copied.region]["transfer"] = {
        {      "source",      route.source },
        { "destination", route.destination },
        {        "kind",              kind }
    };
    for (const auto & slice : slices) {
        graph_.regions[copied.region]["transfer"]["slices"].push_back({
            {                    "kind",                    slice.kind },
            {                   "layer",                   slice.layer },
            {              "head_begin",              slice.head_begin },
            {                "head_end",                slice.head_end },
            {      "source_head_offset",      slice.source_head_offset },
            { "destination_head_offset", slice.destination_head_offset },
            {              "head_bytes",              slice.head_bytes }
        });
    }
    return graph_.execution.region_call(copied.region).returned;
}

void DisaggregatedStructure::transfer(const ServingAction & action, Handoff & handoff) {
    const auto & pages = action.work->transferred_pages;
    const auto page_size = prefill_.sources().front()->environment().at("kv_layout").at("page_size").get<std::int64_t>();
    std::int64_t runs = 0;
    for (std::size_t page = 0; page < pages.size(); ++page) {
        if (page == 0 || pages[page].source != pages[page - 1].source + 1 || pages[page].destination != pages[page - 1].destination + 1) ++runs;
    }

    for (const auto & route : transfers_.copies()) {
        auto ready = handoff.prepared.at(route.source);
        const auto & reserved = handoff.reserved.at(route.destination);
        ready.insert(ready.end(), reserved.begin(), reserved.end());
        std::vector<std::size_t> copied;
        std::int64_t bulk_bytes = 0;
        for (const auto & slice : route.slices) {
            const auto bytes = (slice.head_end - slice.head_begin) * slice.head_bytes * page_size * static_cast<std::int64_t>(pages.size());
            kv_bytes_ += bytes;
            if (route.per_buffer_tokens) {
                copied.push_back(append_transfer(route, bytes, static_cast<std::int64_t>(pages.size()) * page_size, ready, "kv", { &slice, 1 }));
            }
            else bulk_bytes += bytes;
        }
        if (!route.per_buffer_tokens)
            copied.push_back(append_transfer(route, bulk_bytes, runs * static_cast<std::int64_t>(route.slices.size()), ready, "kv", route.slices));

        // Metadata acknowledges every KV fragment for this source/destination pair.
        const auto bytes = std::accumulate(route.metadata_item_bytes.begin(), route.metadata_item_bytes.end(), std::int64_t{ 0 });
        const auto done = append_transfer(route, bytes, static_cast<std::int64_t>(route.metadata_item_bytes.size()), copied, "metadata", {});
        metadata_bytes_ += bytes;
        handoff.transferred[route.source].push_back(done);
        handoff.transferred[route.destination].push_back(done);
    }
}

void DisaggregatedStructure::bind_readiness(const ServingAction & action, const BatchProgram & program, std::span<const std::size_t> drained,
                                            RankPorts & ports) {
    const auto & members = topology_.instance(action.task.group).members;
    if (members.size() != program.completions.size()) throw std::logic_error("PD readiness must retain one completion per instance rank");
    for (std::size_t index = 0; index < members.size(); ++index) ports[members[index]].push_back(program.completions[index]);

    // CPU return and device readiness differ; retain local tails and each communication's actual participants.
    for (const auto node : drained) {
        const auto & owner = graph_.execution.nodes()[node - graph_.execution.first_node()].owner;
        const auto region = owner.kind == ExecutionOwner::Kind::Region ? owner.identity : graph_.operations.at(owner.identity).region;
        const auto & scope = graph_.regions.at(region);
        std::set<std::size_t> participants;
        if (scope.contains("rank")) participants.insert(scope.at("rank").get<std::size_t>());
        else {
            for (const auto & operation : scope.at("rank_operations")) {
                const auto local_region = graph_.operations.at(operation.get<std::size_t>()).region;
                participants.insert(graph_.regions.at(local_region).at("rank").get<std::size_t>());
            }
        }
        for (const auto rank : participants) ports[rank].push_back(node);
    }
    for (auto & [rank, ready] : ports) {
        std::ranges::sort(ready);
        ready.erase(std::unique(ready.begin(), ready.end()), ready.end());
    }
}

BatchProgram DisaggregatedStructure::expand(const ServingAction & action) {
    const auto operation_begin = graph_.operations.size();
    const auto node_begin = graph_.execution.first_node() + graph_.execution.nodes().size();
    const auto dependency_begin = graph_.execution.dependencies().size();
    const auto region_begin = graph_.regions.size();
    BatchProgram result{ std::nullopt, {}, {} };

    if (action.step == ServingStep::Transfer) transfer(action, handoffs_.at(action.work->batch.members.front().request));
    else {
        auto & instance = action.task.group == groups_.prefill ? prefill_ : decode_;
        const bool consumes_transfer = action.step == ServingStep::Accept || action.step == ServingStep::Release;
        RankPorts prerequisites;
        if (consumes_transfer) {
            for (const auto & member : action.work->batch.members) {
                for (const auto & [rank, ports] : handoffs_.at(member.request).transferred)
                    prerequisites[rank].insert(prerequisites[rank].end(), ports.begin(), ports.end());
            }
        }
        result = instance.expand(action, prerequisites);
    }

    auto drained = graph_.execution.terminal_nodes(node_begin);
    if (action.step != ServingStep::Batch) {

        // Batch membership may change between P computation and D admission; request identity is stable.
        for (const auto & member : action.work->batch.members) {
            auto & handoff = handoffs_[member.request];
            if (action.step == ServingStep::Reserve || action.step == ServingStep::Model)
                bind_readiness(action, result, drained, action.step == ServingStep::Reserve ? handoff.reserved : handoff.prepared);
            if (action.step == ServingStep::Accept) handoff.accepted = true;
            if (action.step == ServingStep::Release) handoff.released = true;
            if (handoff.accepted && handoff.released) handoffs_.erase(member.request);
        }
    }

    // PD actions commit whole pool/transfer effects after their own submitted work has drained.
    result.completions = std::move(drained);

    for (auto region = region_begin; region < graph_.regions.size(); ++region) {
        graph_.regions[region]["batch"] = action.flow;
        graph_.regions[region]["task"] = {
            {    "group",    action.task.group },
            { "sequence", action.task.sequence }
        };
    }
    if (action.step == ServingStep::Model || action.step == ServingStep::Batch) ++batches_;
    ++actions_;
    count(operation_begin, node_begin, dependency_begin);
    return result;
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
    auto result = decode_.idle();
    count(operation_begin, node_begin, dependency_begin);
    return result.empty() ? result : graph_.execution.terminal_nodes(node_begin);
}

const RegionGraph & DisaggregatedStructure::graph() const { return graph_; }

std::vector<std::size_t> DisaggregatedStructure::retire() {
    GraphRoots roots;
    prefill_.live_state(roots);
    decode_.live_state(roots);
    auto nodes = graph_.retire(std::move(roots));

    // A peer may become ready before the next control action; retain that time across IR compaction.
    for (const auto & [_, handoff] : handoffs_) {
        for (const auto * ports : { &handoff.reserved, &handoff.prepared }) {
            for (const auto & [rank, ready] : *ports) nodes.insert(nodes.end(), ready.begin(), ready.end());
        }
        for (const auto & [rank, ready] : handoff.transferred) nodes.insert(nodes.end(), ready.begin(), ready.end());
    }
    return nodes;
}

nlohmann::json DisaggregatedStructure::summary() const {
    auto sources = nlohmann::json::array();
    for (const auto * instance : { &prefill_, &decode_ }) {
        const auto summary = instance->summary();
        for (const auto & source : summary.at("rank_sources")) sources.push_back(source);
    }
    return {
        {     "framework",                                              "sglang"                          },
        {          "mode",                                                                 "disaggregated" },
        {      "topology",                                                 topology_.parallel().document() },
        {         "model", prefill_.sources().front()->environment().at("implementation").at("model_path") },
        {  "rank_sources",                                                              std::move(sources) },
        {        "counts",
         { { "batches", batches_ },
         { "actions", actions_ },
         { "operations", operations_ },
         { "execution_nodes", nodes_ },
         { "execution_dependencies", dependencies_ },
         { "kv_bytes", kv_bytes_ },
         { "metadata_bytes", metadata_bytes_ } }                                                          },
        { "qualification",
         { { "scheduling", "controller_selected_batches" },
         { "initial_state", "prewarmed_drained" },
         { "completion", "action_work_drained" },
         { "transfer", "native_synchronous_sdma_interface" },
         { "token_values", "symbolic" },
         { "costs", "given_execution_kind_ns" } }                                                         }
    };
}

nlohmann::json DisaggregatedStructure::document() const {
    auto result = graph_.document();
    result["summary"] = summary();
    return result;
}

} // namespace markov::sglang
