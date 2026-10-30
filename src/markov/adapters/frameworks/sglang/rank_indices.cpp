#include "markov/adapters/frameworks/sglang/rank_structure.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace markov::sglang {

RankProgram RankStructure::indices(const IndexWork & work) {
    static const std::map<IndexService, std::string> services{
        {          IndexService::Match,         "cache_match_prefix" },
        {         IndexService::Insert,               "cache_insert" },
        {     IndexService::InsertPath,        "cache_insert_helper" },
        {          IndexService::Split,           "cache_split_node" },
        {        IndexService::Acquire,         "cache_inc_lock_ref" },
        {        IndexService::Release,         "cache_dec_lock_ref" },
        {  IndexService::RetainRequest, "cache_cache_unfinished_req" },
        { IndexService::ReleaseRequest,   "cache_cache_finished_req" },
        {          IndexService::Evict,                "cache_evict" }
    };
    const auto begin = shared_.regions.size();
    std::vector<std::size_t> calls;
    std::optional<std::size_t> completion;
    std::vector<std::size_t> returned;
    for (const auto & operation : work.operations) {
        if (const auto * call = std::get_if<IndexCall>(&operation)) {
            if (call->enter) {
                if (call->service == IndexService::EmptyMatch) {
                    const auto empty = graph_.append("cache_empty", "control", {});
                    const auto value = empty.ports.outputs.at("prefix_indices");
                    graph_.bind_integers(value, {});
                    index_values_.insert_or_assign(0, value);
                    calls.push_back(empty.region);
                }
                else calls.push_back(graph_.begin_composition(services.at(call->service), "control", {}, {}));
            }
            else {
                const auto region = calls.back();
                if (call->service != IndexService::EmptyMatch) graph_.end_composition({});
                calls.pop_back();
                completion = shared_.execution.region_call(region).returned;
                if (call->service == IndexService::ReleaseRequest) cache_release_ = region;
            }
            continue;
        }

        TargetRegion region;
        if (const auto * read = std::get_if<IndexRead>(&operation)) {
            Invocation call;
            call.parameters = {
                { "request_slot", static_cast<std::int64_t>(read->slot) + 1 },
                {  "index_begin",                               read->begin },
                { "index_tokens",                              read->tokens }
            };
            region = graph_.append("cache_map_read",
                                   "control",
                                   call,
                                   {
                                       { "indices", request_map_.value() }
            });
            IntegerData mapping;
            for (const auto page : read->pages) mapping.append((static_cast<std::int64_t>(page) + 1) * settings_.page_size, settings_.page_size);
            const auto output = region.ports.outputs.at("indices");
            graph_.bind_integers(output, mapping.slice(read->begin, read->tokens));
            index_values_.emplace(read->output, output);
        }
        else if (const auto * slice = std::get_if<IndexSlice>(&operation)) {
            const auto input = index_values_.at(slice->input);
            Invocation call;
            call.parameters = {
                {  "index_begin",  slice->begin },
                { "index_tokens", slice->tokens }
            };
            region = graph_.append("cache_index_slice",
                                   "control",
                                   call,
                                   {
                                       { "indices", input }
            });
            const auto output = region.ports.outputs.at("indices");
            graph_.bind_integers(output, graph_.integers(input).slice(slice->begin, slice->tokens));
            index_values_.emplace(slice->output, output);
        }
        else if (const auto * copy = std::get_if<IndexCopy>(&operation)) {
            const auto input = index_values_.at(copy->input);
            Invocation call;
            call.parameters = {
                { "index_tokens", graph_.integers(input).size() }
            };
            region = graph_.append("cache_index_copy",
                                   "control",
                                   call,
                                   {
                                       { "indices", input }
            });
            const auto output = region.ports.outputs.at("indices");
            graph_.bind_integers(output, graph_.integers(input));
            index_values_.emplace(copy->output, output);
        }
        else if (const auto * concat = std::get_if<IndexConcat>(&operation)) {
            Invocation call;
            GroupValues groups;
            IntegerData contents;
            std::map<std::string, std::size_t> inputs;
            const auto & dtype = shared_.types.at(index_values_.at(concat->inputs.front())).at("dtype");
            const bool uniform =
                std::ranges::all_of(concat->inputs, [&](auto handle) { return shared_.types.at(index_values_.at(handle)).at("dtype") == dtype; });
            for (const auto handle : concat->inputs) {
                const auto input = index_values_.at(handle);
                const auto & data = graph_.integers(input);
                if (uniform) {
                    groups["segments"].push_back(input);
                    call.input_groups["segments"].push_back({
                        { "extent", data.size() }
                    });
                }
                else {

                    // Native mixed-dtype concatenation declares ordered ports for type promotion.
                    const auto position = std::to_string(inputs.size());
                    inputs.emplace("segment_" + position, input);
                    call.parameters.emplace("extent_" + position, data.size());
                }
                contents.append(data);
            }
            region = graph_.append("concatenate", "control", call, inputs, groups);
            const auto output = region.ports.outputs.at("result");
            graph_.bind_integers(output, std::move(contents));
            index_values_.emplace(concat->output, output);
        }
        else if (const auto * write = std::get_if<IndexWrite>(&operation)) {
            Invocation call;
            call.parameters = {
                { "request_slot", static_cast<std::int64_t>(write->slot) + 1 },
                {  "index_begin",                               write->begin },
                { "index_tokens",                              write->tokens }
            };
            region = graph_.append("cache_map_write",
                                   "control",
                                   call,
                                   {
                                       { "request_map",           request_map_.value() },
                                       {     "indices", index_values_.at(write->input) }
            });
            request_map_ = region.ports.outputs.at("request_map");
        }
        else if (const auto * release = std::get_if<IndexRelease>(&operation)) {
            const auto bucket = page_reuse_ == PageReuse::DeferredSorted ? release_pages_ : free_pages_;
            const bool grouped = release->mode == IndexReleaseMode::Group;
            const bool immediate = release->mode != IndexReleaseMode::Deferred;
            Invocation call;
            std::map<std::string, std::size_t> inputs;
            GroupValues groups;
            std::int64_t tokens = 0;
            for (const auto handle : release->inputs) {
                const auto input = index_values_.at(handle);
                const auto size = graph_.integers(input).size();
                tokens += size;
                if (grouped) {
                    groups["segments"].push_back(input);
                    call.input_groups["segments"].push_back({
                        { "extent", size }
                    });
                }
                else inputs.emplace("release_indices", input);
            }
            call.parameters = {
                {                        "released_tokens",                                           tokens },
                {                         "released_pages", static_cast<std::int64_t>(release->pages.size()) },
                {                      "release_immediate",                                        immediate },
                {                  "pending_tokens_before",                   release->pending_tokens_before },
                { std::string(released_pool()) + "_before",     bucket ? graph_.integers(*bucket).size() : 0 }
            };
            call.bindings = {
                { "page_size",                      settings_.page_size },
                { "need_sort", page_reuse_ == PageReuse::DeferredSorted }
            };
            if (bucket && immediate && tokens != 0) inputs.emplace(released_pool(), *bucket);
            region = graph_.append(grouped ? "kv_release_group" : "kv_release", "control", call, inputs, groups);
            if (immediate && tokens != 0) {
                if (!bucket) graph_.bind_integers(region.ports.inputs.at(released_pool()), {});
                return_pages(region.ports.outputs.at(released_pool()), release->pages);
                returned.insert(returned.begin(), release->pages.begin(), release->pages.end());
            }
        }
        completion = shared_.execution.region_call(region.region).returned;
    }
    if (!calls.empty() || !completion) throw std::invalid_argument("index work requires closed native services and execution work");

    if (work.prefix) chunks_[work.prefix->request].retained = index_values_.at(work.prefix->indices);
    const std::set live(work.live_indices.begin(), work.live_indices.end());
    std::erase_if(index_values_, [&](const auto & item) { return !live.contains(item.first); });
    stamp(begin);
    return { std::nullopt, *completion, std::move(returned) };
}

} // namespace markov::sglang
