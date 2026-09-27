#include "markov/trace_graph/modules/hicache/model/detail/state_model_helpers.hpp"

#include <set>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::model {

void HiCacheState::apply_cache_decode_allocation(const HiCacheFact & fact) {
    if (!execution_prefetch_control_ || fact.batch_paths.empty() || fact.batch_paths.size() != fact.decode_iterations.size())
        throw std::invalid_argument("Decode allocation requires a complete execution action");
    auto & scope = scope_state(fact);
    const auto page_size = pager_.page_size_for_fact(fact);
    ensure_device_allocator(scope);
    struct Demand {
        std::string key;
        uint64_t pages;
    };
    std::vector<Demand> demands;
    std::set<std::string> requests;
    uint64_t new_pages = 0;
    for (size_t i = 0; i < fact.batch_paths.size(); ++i) {
        auto request_fact = fact;
        request_fact.request_id = fact.batch_paths[i].request_id;
        const auto key = scoped_request_key(request_fact);
        const auto found = scope.requests.find(key);
        if (!requests.insert(key).second || found == scope.requests.end() || !found->second.extended_tokens
            || (found->second.lifecycle_state != "extended" && found->second.lifecycle_state != "unfinished"))
            throw std::runtime_error("Decode allocation has no unique admitted request");
        const auto & request = found->second;
        if (request.decode_iteration != fact.decode_iterations[i]) throw std::runtime_error("Decode allocation iteration is missing or repeated");
        const auto length = core::checked_add_u64(request.extended_tokens, request.decoded_tokens, "Decode length overflow");
        const auto next_length = core::checked_add_u64(length, 1, "Decode next length overflow");
        const auto pages = ceil_div(next_length, page_size) - ceil_div(length, page_size);
        new_pages = core::checked_add_u64(new_pages, pages, "Decode batch page count overflow");
        demands.push_back({ key, pages });
    }
    // SGLang checks a one-page-per-request budget on EVERY iteration. The NPU
    // allocator then merges/releases and consumes only the actual new pages.
    const auto budget = demands.size();
    allocate_after_capacity(fact, scope, budget, [this, &scope, demands = std::move(demands), new_pages](const HiCacheFact &) {
        scope.device_allocator.merge_before_page_allocation(new_pages);
        if (!scope.device_allocator.can_allocate(new_pages)) throw std::runtime_error("Target decode allocation is out of device pages");
        for (const auto & demand : demands) {
            auto & request = scope.requests.at(demand.key);
            request.kv_allocated_pages =
                core::checked_add_u64(request.kv_allocated_pages, scope.device_allocator.allocate(demand.pages), "Decode request page count overflow");
            ++request.decoded_tokens;
            ++request.decode_iteration;
        }
    });
}

} // namespace markov::trace_graph::modules::hicache::model
