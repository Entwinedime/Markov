#include "markov/trace_graph/modules/hicache/model/detail/state_model_helpers.hpp"

#include <set>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::model {

void HiCacheState::apply_cache_decode_allocation(const HiCacheFact & fact) {
    if ((formal_window_active_ && !execution_prefetch_control_) || fact.batch_paths.empty() || fact.batch_paths.size() != fact.decode_iterations.size())
        throw std::invalid_argument("Decode allocation requires a complete execution action");
    auto & scope = scope_state(fact);
    const auto page_size = pager_.page_size_for_fact(fact);
    ensure_device_allocator(scope);
    struct Demand {
        std::string key;
        uint64_t pages;
    };
    std::vector<Demand> demands;
    HiCacheAllocatorWorkItem allocation{
        .phase = HiCacheAllocatorPhase::Decode,
        .source_fact_id = fact.source_node_id,
        .source_event_index = fact.source_event_index,
        .pid = fact.pid,
        .formal = formal_window_active_,
        .page_size = page_size,
        .batch_size = fact.batch_paths.size(),
        .extend_tokens = fact.batch_paths.size(),
    };
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
        allocation.requests.push_back({ fact.batch_paths[i].request_id, next_length, length, fact.decode_iterations[i] });
    }
    // SGLang checks a one-page-per-request budget on EVERY iteration. The NPU
    // allocator then merges/releases and consumes only the actual new pages.
    const auto budget = demands.size();
    allocation.allocated_pages = new_pages;
    allocate_after_capacity(fact,
                            scope,
                            budget,
                            [this, &scope, demands = std::move(demands), allocation = std::move(allocation), new_pages](const HiCacheFact &) mutable {
                                allocation.queue_state_known = scope.device_allocator.capacity_pages > 0;
                                allocation.free_pages_before = scope.device_allocator.free_pages;
                                allocation.release_pages_before = scope.device_allocator.release_pages;
                                allocation.merge_release_queue = new_pages > allocation.free_pages_before && allocation.release_pages_before > 0;

                                scope.device_allocator.merge_before_page_allocation(new_pages);
                                if (!scope.device_allocator.can_allocate(new_pages))
                                    throw std::runtime_error("Target decode allocation is out of device pages");
                                allocation.free_index_offset = scope.device_allocator.free_index_offset;
                                allocator_work_items_.push_back(std::move(allocation));

                                for (const auto & demand : demands) {
                                    auto & request = scope.requests.at(demand.key);
                                    request.kv_allocated_pages = core::checked_add_u64(request.kv_allocated_pages,
                                                                                       scope.device_allocator.allocate(demand.pages),
                                                                                       "Decode request page count overflow");
                                    ++request.decoded_tokens;
                                    ++request.decode_iteration;
                                }
                            });
}

} // namespace markov::trace_graph::modules::hicache::model
