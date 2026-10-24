#include "markov/adapters/frameworks/sglang/rank_structure.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace markov::sglang {
namespace {

std::int64_t page_base(std::size_t page, std::int64_t page_size) {

    // The native allocator reserves page zero; logical identities stay zero-based in the controller.
    return (static_cast<std::int64_t>(page) + 1) * page_size;
}

IntegerData page_indices(std::span<const std::size_t> pages, std::int64_t tokens, std::int64_t page_size) {
    IntegerData result;
    for (const auto page : pages) {
        const auto count = std::min(tokens, page_size);
        result.append(page_base(page, page_size), count);
        tokens -= count;
    }

    return result;
}

std::map<std::string, IntegerData> batch_operands(const Batch & batch, std::int64_t page_size) {
    std::map<std::string, IntegerData> result;
    for (const auto & member : batch.members) {
        for (const auto * name : { "request_slots", "request_slots_cpu" }) result[name].append(static_cast<std::int64_t>(member.slot) + 1);
        for (const auto * name : { "sequence_lengths", "sequence_lengths_cpu" }) result[name].append(member.context_tokens + member.new_tokens);
        for (const auto * name : { "prefix_lengths", "prefix_lengths_cpu" }) result[name].append(member.context_tokens);

        const auto last = member.context_tokens == 0 ? -1 : page_base(member.previous_page.value(), page_size) + (member.context_tokens - 1) % page_size;

        // Fill the current page tail before consuming newly allocated pages.
        const auto tail = member.context_tokens % page_size;
        const auto reused = tail == 0 ? 0 : std::min(member.new_tokens, page_size - tail);
        result["kv_locations"].append(last + 1, reused);
        auto added = page_indices(member.new_pages, member.new_tokens - reused, page_size);
        result["kv_locations"].append(std::move(added));
    }

    return result;
}

void bind_columns(TargetGraph & graph, const std::map<std::string, std::size_t> & ports, const std::map<std::string, IntegerData> & columns) {
    for (const auto & [name, value] : ports) {
        if (const auto column = columns.find(name); column != columns.end()) graph.bind_integers(value, column->second);
    }
}

} // namespace

RankStructure::RankStructure(const TemplateAsset & asset, ServingSettings settings, RegionGraph & graph, std::size_t execution_offset, std::size_t rank)
    : settings_(settings),
      shared_(graph),
      graph_(asset, graph, execution_offset),
      views_(graph_),
      rank_(rank),
      publishes_(std::ranges::any_of(asset.entrypoints(), [](const RegionSelection & entry) { return entry.role == "request_output"; })) {}

RankProgram RankStructure::expand(const Batch & batch, const BatchEffects & effects) {
    const auto begin = graph_.graph().regions.size();
    const auto sampled_tokens = prepare(batch, effects);
    auto result = complete(batch, effects, sampled_tokens);
    for (std::size_t region = begin; region < shared_.regions.size(); ++region) shared_.regions[region]["rank"] = rank_;
    return result;
}

std::size_t RankStructure::prepare(const Batch & batch, const BatchEffects & effects) {
    const auto columns = batch_operands(batch, settings_.page_size);
    const auto phase = batch.phase == Phase::Prefill ? "prefill" : "decode";
    std::map<std::string, std::size_t> state;
    if (free_pages_) state.emplace("free_pages", *free_pages_);
    if (request_map_) state.emplace("request_map", *request_map_);
    graph_.begin_composition("Schedule",
                             phase,
                             {
                                 { "scheduled_members", static_cast<std::int64_t>(batch.members.size()) }
    },
                             state);
    views_.advance(batch.phase);

    std::int64_t max_sequence_tokens = 0;
    for (const auto & member : batch.members) max_sequence_tokens = std::max(max_sequence_tokens, member.context_tokens + member.new_tokens);

    Invocation invocation;
    invocation.parameters = {
        {                   "M",                              batch.new_tokens() },
        {                   "B", static_cast<std::int64_t>(batch.members.size()) },
        {                   "R",                  settings_.max_running_requests },
        {   "free_pages_before",                         batch.free_pages_before },
        {     "allocated_pages",                           batch.allocated_pages },
        { "max_sequence_tokens",                             max_sequence_tokens }
    };
    invocation.bindings = {
        { "page_size", settings_.page_size }
    };

    GroupValues groups;

    // Prefix lookup returns actual producer values, including the cold-cache empty case.
    if (batch.phase == Phase::Prefill) {
        for (std::size_t member = 0; member < batch.members.size(); ++member) {
            const auto prefix = graph_.append("prefix_match", phase, {});
            groups["prefixes"].push_back(prefix.ports.outputs.at("prefix_indices"));
        }
        invocation.input_groups["prefixes"] = std::vector<Parameters>(batch.members.size(),
                                                                      {
                                                                          { "extent", 0 }
        });

        auto & allocation = invocation.sequences["allocation_members"];
        std::int64_t page_offset = 0;
        std::int64_t token_offset = 0;
        for (std::size_t index = 0; index < batch.members.size(); ++index) {
            const auto & member = batch.members[index];
            allocation.push_back({
                {   "member_index", static_cast<std::int64_t>(index) },
                {     "new_tokens",                member.new_tokens },
                { "context_tokens",            member.context_tokens },
                {    "page_offset",                      page_offset },
                {   "token_offset",                     token_offset }
            });
            page_offset += static_cast<std::int64_t>(member.new_pages.size());
            token_offset += member.new_tokens;
        }
    }

    std::map<std::string, std::size_t> inputs;
    if (batch.phase == Phase::Decode) inputs = views_.decode_inputs();
    if (free_pages_) inputs.emplace("free_pages", *free_pages_);
    if (request_map_) inputs.emplace("request_map", *request_map_);
    const auto preparation = graph_.append("batch_preparation", phase, invocation, inputs, groups);
    const auto & prepared = preparation.ports;
    if (!free_pages_) {
        IntegerData initial;
        initial.append(1, batch.free_pages_before);
        graph_.bind_integers(prepared.inputs.at("free_pages"), std::move(initial));
    }

    bind_columns(graph_, prepared.inputs, columns);
    bind_columns(graph_, prepared.outputs, columns);
    if (batch.phase == Phase::Decode) graph_.bind_integers(prepared.inputs.at("original_lengths"), columns.at("prefix_lengths"));
    graph_.bind_integers(prepared.outputs.at("original_lengths"), columns.at("sequence_lengths"));
    for (const auto & [_, members] : preparation.group_inputs) {
        for (const auto prefix : members) graph_.bind_integers(prefix, {});
    }
    graph_.bind_integers(prepared.outputs.at("free_pages"),
                         graph_.integers(prepared.inputs.at("free_pages")).slice(batch.allocated_pages, batch.free_pages_before - batch.allocated_pages));
    free_pages_ = prepared.outputs.at("free_pages");
    request_map_ = prepared.outputs.at("request_map");
    graph_.end_composition(prepared.outputs);

    // These are the actual prepared values, including the CPU length vector and prompt staging buffer.
    inputs = BatchViews::columns(prepared.outputs);
    inputs.emplace("kv_locations", prepared.outputs.at("kv_locations"));
    if (batch.phase == Phase::Prefill) inputs.emplace("prompt_tokens", prepared.outputs.at("prompt_tokens"));
    if (token_relay_) inputs.emplace("token_relay", *token_relay_);

    invocation.input_groups.clear();
    invocation.sequences.clear();
    const auto model = graph_.append(batch.phase == Phase::Prefill ? "Prefill" : "Decode", phase, invocation, inputs);
    token_relay_ = model.ports.outputs.at("token_relay");
    views_.remember(batch.phase, model.ports.outputs, effects);
    return model.ports.outputs.at("sampled_tokens");
}

RankProgram RankStructure::complete(const Batch & batch, const BatchEffects & effects, std::size_t sampled_tokens) {
    const bool immediate = batch.phase == Phase::Prefill;
    const auto phase = immediate ? "prefill" : "decode";
    Invocation group;
    GroupValues group_values;
    std::vector<std::size_t> returned_pages;
    std::int64_t pending_tokens = 0;

    const auto result_region =
        graph_.begin_composition("ResultProcessed",
                                 phase,
                                 {
                                     { "result_members", static_cast<std::int64_t>(effects.members.size()) }
    },
                                 { { "sampled_tokens", sampled_tokens }, { "free_pages", free_pages_.value() }, { "request_map", request_map_.value() } });

    Invocation result_call;
    result_call.parameters = {
        { "B", static_cast<std::int64_t>(batch.members.size()) }
    };
    const std::map<std::string, std::size_t> result_input{
        { "tensor", sampled_tokens }
    };
    graph_.append("result_tokens", phase, result_call, result_input);

    for (const auto & member : effects.members) {
        if (!member.finished && !immediate) continue;

        Invocation invocation;
        invocation.parameters = {
            {               "R",             settings_.max_running_requests },
            {    "request_slot", static_cast<std::int64_t>(member.slot) + 1 },
            { "resident_tokens",                     member.resident_tokens }
        };
        std::map<std::string, std::size_t> inputs{
            { "request_map", request_map_.value() }
        };
        if (member.finished) {
            invocation.parameters.insert({
                {     "release_immediate",                                               immediate },
                { "pending_tokens_before",                                          pending_tokens },
                {     "free_pages_before",                    graph_.integers(*free_pages_).size() },
                {        "released_pages", static_cast<std::int64_t>(member.released_pages.size()) }
            });
            invocation.bindings = {
                { "page_size", settings_.page_size },
                { "need_sort",                   0 }
            };
            if (immediate) inputs.emplace("free_pages", *free_pages_);
        }

        const auto cache = graph_.append(member.finished ? "request_cache_release" : "request_cache_retain", phase, invocation, inputs);
        if (!member.finished) continue;

        // These contents come from controller ownership; the view keeps its native map producer and dependencies.
        const auto indices = cache.ports.outputs.at("request_indices");
        graph_.bind_integers(indices, page_indices(member.released_pages, member.resident_tokens, settings_.page_size));
        if (immediate) {
            auto pages = member.released_pages;
            std::ranges::sort(pages);
            return_pages(cache.ports.outputs.at("free_pages"), pages);
            returned_pages.insert(returned_pages.begin(), pages.begin(), pages.end());
        }
        else {
            group.input_groups["segments"].push_back({
                { "extent", member.resident_tokens }
            });
            group_values["segments"].push_back(indices);
            pending_tokens += member.resident_tokens;
            returned_pages.insert(returned_pages.end(), member.released_pages.begin(), member.released_pages.end());
        }
    }

    // Native publication precedes the Decode group's final page-pool update.
    std::optional<std::size_t> publication;
    if (publishes_) {
        result_call.parameters["published_requests"] = std::ranges::count_if(effects.members, &RequestCompletion::emits_output);
        const auto region = graph_.append("request_output", phase, result_call).region;
        publication = graph_.graph().execution.region_call(region).returned;
    }

    // Decode accumulates members before one native unique/concatenate update of the pool.
    if (!group_values.empty()) {
        std::ranges::sort(returned_pages);
        group.parameters = {
            { "free_pages_before",             graph_.integers(*free_pages_).size() },
            {    "released_pages", static_cast<std::int64_t>(returned_pages.size()) }
        };
        group.bindings = {
            { "page_size", settings_.page_size }
        };
        const std::map<std::string, std::size_t> inputs{
            { "free_pages", *free_pages_ }
        };
        const auto release = graph_.append("kv_release_group", phase, group, inputs, group_values);
        return_pages(release.ports.outputs.at("free_pages"), returned_pages);
    }

    graph_.end_composition({
        { "free_pages", free_pages_.value() }
    });
    return { publication, graph_.graph().execution.region_call(result_region).returned, std::move(returned_pages) };
}

std::size_t RankStructure::admission_members() const { return views_.admission_members(); }

void RankStructure::return_pages(std::size_t output, std::span<const std::size_t> pages) {
    IntegerData returned;
    for (const auto page : pages) returned.append(static_cast<std::int64_t>(page) + 1);
    returned.append(graph_.integers(*free_pages_));
    graph_.bind_integers(output, std::move(returned));
    free_pages_ = output;
}

void RankStructure::live_state(std::vector<std::size_t *> & values, std::set<std::size_t> & storage) {

    // References remain stable while the shared graph compacts its values and rebinds these ports.
    for (auto * value : { &free_pages_, &request_map_, &token_relay_ }) {
        if (*value) values.push_back(&value->value());
    }
    views_.live_values(values);
    graph_.live_resources(storage);
}

std::optional<std::size_t> RankStructure::idle() {
    const auto begin = shared_.regions.size();
    const auto control = views_.idle();
    for (std::size_t region = begin; region < shared_.regions.size(); ++region) shared_.regions[region]["rank"] = rank_;
    if (!control) return std::nullopt;
    return shared_.execution.region_call(*control).returned;
}

} // namespace markov::sglang
