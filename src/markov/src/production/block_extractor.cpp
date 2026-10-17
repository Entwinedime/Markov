#include "markov/production/block_extractor.hpp"

#include "markov/production/block_binding.hpp"
#include "markov/production/source_bindings.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace markov {

BlockExtractor::BlockExtractor(const SourceGraph & graph, std::span<const SourceCall> calls, const std::vector<std::vector<std::size_t>> & native_api_members,
                               bool native_dense_path, const SourceBindings & bindings)
    : graph_(graph),
      calls_(calls),
      native_api_members_(native_api_members),
      native_dense_path_(native_dense_path),
      bindings_(bindings) {}

nlohmann::json BlockExtractor::extract_regions() const {
    // Regions reference existing spans and cut boundaries. They do not create
    // executable work, completion barriers, or another CPU charge.
    std::vector<std::size_t> member_counts(calls_.size());
    std::map<BoundaryId, std::set<std::size_t>> users;
    for (SourceSpanId identity = 0; identity < graph_.spans().size(); ++identity) {
        const auto & span = graph_.spans()[identity];
        auto scope = span.scope_call;
        while (scope && calls_[*scope].is_annotation()) {
            ++member_counts[*scope];
            users[span.begin].insert(*scope);
            users[span.end].insert(*scope);
            scope = calls_[*scope].parent;
        }
    }

    // Completion joins have no service span. They belong to a region only when
    // every completed span belongs to it; a cross-region join stays external.
    // SourceGraph::complete connects span ends directly, so no recursive closure
    // or temporal containment is needed to establish this membership.
    const std::set<std::size_t> empty;
    std::map<BoundaryId, std::set<std::size_t>> completion_users;
    for (const auto & edge : graph_.edges()) {
        if (edge.relation != SourceRelation::CompletionJoin) continue;
        const auto source = users.find(edge.source);
        const auto & regions = source == users.end() ? empty : source->second;
        auto [position, inserted] = completion_users.try_emplace(edge.target, regions);
        if (!inserted) std::erase_if(position->second, [&](std::size_t region) { return !regions.contains(region); });
    }
    for (auto & [boundary, regions] : completion_users) users[boundary].insert(regions.begin(), regions.end());

    std::vector<std::size_t> edge_counts(calls_.size());
    std::vector<std::set<BoundaryId>> inputs(calls_.size());
    std::vector<std::set<BoundaryId>> outputs(calls_.size());
    for (std::size_t index = 0; index < graph_.edges().size(); ++index) {
        const auto & edge = graph_.edges()[index];
        const auto source = users.find(edge.source);
        const auto target = users.find(edge.target);
        const auto & sources = source == users.end() ? empty : source->second;
        const auto & targets = target == users.end() ? empty : target->second;
        for (const auto region : sources) {
            if (targets.contains(region)) ++edge_counts[region];
            else outputs[region].insert(edge.source);
        }
        for (const auto region : targets) {
            if (!sources.contains(region)) inputs[region].insert(edge.target);
        }
    }

    auto regions = nlohmann::json::array();
    for (std::size_t index = 0; index < calls_.size(); ++index) {
        const auto & call = calls_[index];
        if (!call.is_annotation()) continue;

        nlohmann::json scope = nullptr;
        if (call.marker) {
            const auto & marker = graph_.spans()[*call.marker];
            scope = {
                {         "entered", marker.begin },
                { "native_returned",   marker.end }
            };
        }
        const auto serving = call.serving_parent(calls_).value();
        regions.push_back({
            {         "source_call",                                                                  index },
            {         "parent_call",                                                           *call.parent },
            {        "serving_call",                                                                serving },
            {             "binding", call.binding ? nlohmann::json(*call.binding) : nlohmann::json(nullptr) },
            {              "module",        call.region ? call.region->describe() : nlohmann::json(nullptr) },
            {          "span_count",                                                   member_counts[index] },
            { "internal_edge_count",                                                     edge_counts[index] },
            {    "input_boundaries",                                                          inputs[index] },
            {   "output_boundaries",                                                         outputs[index] },
            {           "cpu_scope",                                                                  scope }
        });
    }

    return regions;
}

nlohmann::json BlockExtractor::extract() const {
    std::vector<std::vector<std::size_t>> users(graph_.boundaries().size());
    std::vector<std::vector<SourceSpanId>> members(calls_.size());
    std::vector<std::vector<BoundaryId>> logical_members(calls_.size());
    for (SourceSpanId identity = 0; identity < graph_.spans().size(); ++identity) {
        const auto & span = graph_.spans()[identity];
        if (!span.owner_call) continue;

        members[*span.owner_call].push_back(identity);
        users[span.begin].push_back(*span.owner_call);
        users[span.end].push_back(*span.owner_call);
    }

    for (BoundaryId identity = 0; identity < graph_.boundaries().size(); ++identity) {
        const auto & point = graph_.boundaries()[identity];
        if (point.owner_call) {
            users[identity].push_back(*point.owner_call);
            logical_members[*point.owner_call].push_back(identity);
        }
    }

    for (auto & owners : users) {
        std::sort(owners.begin(), owners.end());
        owners.erase(std::unique(owners.begin(), owners.end()), owners.end());
    }

    // Shared owners retain internal edges; ownership changes expose template boundaries.
    std::vector<std::vector<std::size_t>> internal_edges(calls_.size());
    std::vector<std::set<BoundaryId>> inputs(calls_.size());
    std::vector<std::set<BoundaryId>> outputs(calls_.size());
    auto external_edges = nlohmann::json::array();
    auto unassigned_edges = nlohmann::json::array();
    for (std::size_t index = 0; index < graph_.edges().size(); ++index) {
        const auto & edge = graph_.edges()[index];
        const auto & sources = users[edge.source];
        const auto & targets = users[edge.target];
        if (sources.empty() && targets.empty()) {
            unassigned_edges.push_back(index);
            continue;
        }

        for (const auto owner : sources) {
            if (std::binary_search(targets.begin(), targets.end(), owner)) internal_edges[owner].push_back(index);
            else outputs[owner].insert(edge.source);
        }
        for (const auto owner : targets) {
            if (!std::binary_search(sources.begin(), sources.end(), owner)) inputs[owner].insert(edge.target);
        }

        if (sources == targets) continue;
        external_edges.push_back({
            {     "source_edge",       index },
            { "source_boundary", edge.source },
            { "target_boundary", edge.target },
            {    "source_calls",     sources },
            {    "target_calls",     targets }
        });
    }

    // Project only recorded thread paths whose intermediate points have no
    // modeled owner. Stop at the next owned point, even if it is another block.
    // This preserves ordering without importing excluded CPU work or its cost.
    const auto absent = graph_.boundaries().size();
    std::vector<BoundaryId> thread_next(absent, absent);
    for (const auto & edge : graph_.edges())
        if (edge.relation == SourceRelation::ThreadOrder) thread_next[edge.source] = edge.target;

    std::vector<std::vector<std::array<BoundaryId, 2>>> internal_thread_paths(calls_.size());
    for (BoundaryId begin = 0; begin < absent; ++begin) {
        if (users[begin].empty()) continue;
        auto end = thread_next[begin];
        if (end == absent || !users[end].empty()) continue;

        while (end != absent && users[end].empty()) end = thread_next[end];
        if (end == absent) continue;
        for (const auto owner : users[begin])
            if (std::binary_search(users[end].begin(), users[end].end(), owner)) internal_thread_paths[owner].push_back({ begin, end });
    }

    auto gaps = nlohmann::json::array();
    auto result = nlohmann::json::array();
    for (std::size_t index = 0; index < calls_.size(); ++index) {
        const auto & call = calls_[index];
        if (call.is_annotation()) continue;

        auto parameters = nlohmann::json::array();
        for (const auto & request : call.requests) {
            parameters.push_back({
                {       "request_id",               request.id },
                {     "input_tokens",     request.input_tokens },
                {   "context_tokens",   request.context_tokens },
                {    "prompt_tokens",    request.prompt_tokens },
                { "generated_tokens", request.generated_tokens }
            });
        }

        const auto ports = resolve_ports(index);
        const ServingContract contract;
        for (const auto port : contract.block(call.step).exits) {
            const std::string name(contract.port_name(port));
            if (!ports.contains(name)) {
                gaps.push_back({
                    {   "kind",             "observation" },
                    { "reason", "block_port_not_observed" },
                    {   "call",             call.identity },
                    {   "port",                      name }
                });
            }
        }

        nlohmann::json layout = nullptr;
        if (call.input_layout) {
            layout = {
                {  "shape",  call.input_layout->shape },
                { "stride", call.input_layout->stride },
                {  "dtype",  call.input_layout->dtype },
                { "device", call.input_layout->device }
            };
        }

        // These are source decision/state observations, not instructions for
        // target instantiation. Target visibility and completion are recomputed.
        auto source_state = nlohmann::json::object();

        if (call.step == StepKind::Schedule) {
            source_state["visible_requests"] = call.visible_requests;
            auto filters = nlohmann::json::array();
            for (const auto & filter : call.batch_filters)
                filters.push_back({
                    { "before", filter.before },
                    {  "after",  filter.after }
                });
            source_state["batch_filters"] = std::move(filters);
            if (call.admission_policy) source_state["admission_policy"] = call.admission_policy->describe();
            if (call.worker_controls) source_state["worker_controls"] = call.worker_controls->describe();
        }
        if (call.step == StepKind::Publish) {
            auto publications = nlohmann::json::array();
            for (const auto & publication : call.publications) {
                publications.push_back({
                    {       "request_id",       publication.request_id },
                    { "generated_tokens", publication.generated_tokens },
                    {         "finished",         publication.finished }
                });
            }
            source_state["published_requests"] = std::move(publications);
            if (call.output_policy) {
                source_state["output_policy"] = {
                    { "force_interval_tokens", call.output_policy->force_interval_tokens },
                    {         "non_streaming",         call.output_policy->non_streaming }
                };
            }
        }

        result.push_back({
            {               "binding",                                           call.binding ? nlohmann::json(*call.binding) : nlohmann::json(nullptr) },
            {           "source_call",                                                                                                            index },
            {              "identity",                                                                                                    call.identity },
            {                  "step",                                                                          ServingContract{}.block(call.step).name },
            {                  "rank",                                                                                                        call.rank },
            {              "batch_id",                                         call.batch_id ? nlohmann::json(*call.batch_id) : nlohmann::json(nullptr) },
            {                 "phase", call.phase ? nlohmann::json(*call.phase == BatchPhase::Prefill ? "prefill" : "decode") : nlohmann::json(nullptr) },
            {              "requests",                                                                                                       parameters },
            {          "source_state",                                                                                                     source_state },
            {          "input_layout",                                                                                                           layout },
            {                 "spans",                                                                                                   members[index] },
            { "internal_thread_paths",                                                                                     internal_thread_paths[index] },
            {        "internal_edges",                                                                                            internal_edges[index] },
            {            "native_api",                                                                                       native_api_members_[index] },
            {    "logical_boundaries",                                                                                           logical_members[index] },
            {                 "ports",                                                                                                            ports },
            {      "input_boundaries",                                                                                                    inputs[index] },
            {     "output_boundaries",                                                                                                   outputs[index] },
            {     "observed_start_ns",                                                                                           call.observed_start_ns },
            {    "allowed_transforms",                                                                    nlohmann::json::array({ "request_identity" }) },
            {             "readiness",                                                                                                     "needs_data" }
        });
    }

    auto call_references = nlohmann::json::array();

    for (const auto & call : calls_) {
        call_references.push_back({
            {    "identity",                            call.identity                            },
            {        "rank",                                                            call.rank },
            { "native_step",
             call.kind == SourceCallKind::ForwardInputs ? "model_forward"
             : call.kind == SourceCallKind::NativeRegion
             ? (bindings_.region(call.binding.value()).component_path.empty() ? "model_region" : "component_region")
             : std::string(ServingContract{}.block(call.step).name)                              },
            {      "parent", call.parent ? nlohmann::json(*call.parent) : nlohmann::json(nullptr) }
        });
    }

    auto shared_boundaries = nlohmann::json::array();
    for (BoundaryId identity = 0; identity < users.size(); ++identity) {
        if (users[identity].size() > 1)
            shared_boundaries.push_back({
                { "source_boundary",        identity },
                {    "source_calls", users[identity] }
            });
    }

    return {
        {                 "gaps",                                                                      gaps                                 },
        {  "semantic_resolution",                                           native_dense_path_ ? "native_dense_module" : "observed_subgraph" },
        {                "calls",                                                                                            call_references },
        {              "regions",                                                                                          extract_regions() },
        {            "instances",                                                                                                     result },
        { "cross_template_edges",                                                                                             external_edges },
        {     "unassigned_edges",                                                                                           unassigned_edges },
        {    "shared_boundaries",                                                                                          shared_boundaries },
        {    "parameter_sources",
         { { "input_tokens", "native extend_input_len or decode token" },
         { "context_tokens", "native prefix indices or prompt plus generated minus one" },
         { "deployment", "manifest environment" } }                                                                                         },
        {  "performance_mapping",
         { { "cpu_observation", "nested annotations; never an additional CPU charge" },
         { "cpu_control", "structural serving phases; host CPU service time is excluded" },
         { "host_wait", "native stream synchronization; known predecessors only; observed elapsed is not service" },
         { "compute_and_dma", "physical span candidates; no cost is bound" },
         { "communication", "collective/subtask annotations; not an additional charge" },
         { "device_wait", "observed wait is not service; event/notify producers use native identities; unresolved causes remain gaps" } }   }
    };
}


} // namespace markov
