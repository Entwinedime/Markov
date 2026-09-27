#include "markov/trace_graph/modules/hicache/model/detail/state_model_helpers.hpp"
#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace markov::trace_graph::modules::hicache::model {

const HiCacheLoadAdmissionWork * HiCacheState::load_admission_work(const HiCacheFact & fact) const {
    const auto scope = scopes_.find(normalized_scope(fact));
    if (scope == scopes_.end()) return nullptr;
    const auto work = scope->second.load_admission_work.find(fact.source_node_id);
    return work == scope->second.load_admission_work.end() ? nullptr : &work->second;
}

bool HiCacheState::load_allocation_will_succeed(const HiCacheFact & fact) const {
    const auto * work = load_admission_work(fact);
    if (!work) throw std::logic_error("Load allocation projection needs target admission work");
    if (!allocation_pending(fact)) return work->allocated;
    const auto & scope = scopes_.at(normalized_scope(fact));
    auto allocator = scope.device_allocator;
    for (const auto & victim : scope.allocation->victims) allocator.release(victim.pages.size());
    allocator.merge_before_page_allocation(work->requested_pages);
    return allocator.can_allocate(work->requested_pages);
}

std::vector<std::string> HiCacheState::loadback_dependencies(const HiCacheFact & fact) const {
    if (!execution_prefetch_control_) throw std::logic_error("Load dependencies require execution-driven state");
    const auto & scope = scopes_.at(normalized_scope(fact));
    const auto & request = scope.requests.at(scoped_request_key(fact));
    const std::unordered_set<std::string> pages(request.device_pages.begin(), request.device_pages.end());
    std::vector<std::string> dependencies;
    for (const auto & [id, op] : scope.async_ops.loadback_ops()) {
        if (op.header.state == HiCacheOperationState::Completed || op.header.state == HiCacheOperationState::Committed
            || op.header.state == HiCacheOperationState::Cancelled)
            continue;
        if (std::ranges::any_of(op.header.pages, [&](const auto & page) { return pages.contains(page); })) dependencies.push_back(id);
    }
    std::ranges::sort(dependencies);
    return dependencies;
}

std::optional<HiCacheLoadbackBatch> HiCacheState::submit_loadbacks(const HiCacheFact & fact) {
    if (!execution_prefetch_control_) throw std::logic_error("Explicit load submission requires execution-driven state");
    auto & scope = scope_state(fact);
    if (scope.pending_loadbacks.empty()) return std::nullopt;
    uint64_t pages = 0;
    for (const auto & id : scope.pending_loadbacks) {
        const auto & op = scope.async_ops.loadback_ops().at(id);
        if (fact.ts < op.header.enqueue_ts) throw std::logic_error("Load submission precedes a queued operation");
        pages = core::checked_add_u64(pages, op.header.pages.size(), "Merged load size overflow");
    }
    HiCacheLoadbackBatch batch{
        .id = scope.clock.next_enqueue_epoch(),
        .operations = scope.pending_loadbacks,
        .io_schedule = schedule_target_io(normalized_scope(fact), "load", fact.ts, pages),
    };
    scope.load_ack_queue.push_back(batch);
    scope.pending_loadbacks.clear();
    return batch;
}

void HiCacheState::complete_loadback_batch(const HiCacheFact & fact, uint64_t batch_id) {
    if (!execution_prefetch_control_) throw std::logic_error("Explicit load completion requires execution-driven state");
    auto & scope = scope_state(fact);
    const auto batch = std::ranges::find(scope.load_ack_queue, batch_id, &HiCacheLoadbackBatch::id);
    if (batch == scope.load_ack_queue.end() || batch->completed_at || fact.ts < batch->io_schedule.ready_ts)
        throw std::logic_error("Load completion requires a causal, uncompleted submitted batch");
    batch->completed_at = fact.ts;
    for (const auto & id : batch->operations) scope.async_ops.set_loadback_state(id, HiCacheOperationState::Completed, fact.ts);
}

uint64_t HiCacheState::load_completion_count(const HiCacheFact & fact) const {
    if (!execution_prefetch_control_) throw std::logic_error("Explicit load confirmation requires execution-driven state");
    const auto scope = scopes_.find(normalized_scope(fact));
    if (scope == scopes_.end()) return 0;
    uint64_t count = 0;
    for (const auto & batch : scope->second.load_ack_queue) {
        if (!batch.completed_at || *batch.completed_at > fact.ts) break;
        ++count;
    }
    return count;
}

void HiCacheState::acknowledge_loads(const HiCacheFact & fact, uint64_t batches) {
    if (batches > load_completion_count(fact)) throw std::logic_error("Load confirmation exceeds the completed DMA prefix");
    auto & scope = scope_state(fact);
    for (uint64_t i = 0; i < batches; ++i) {
        const auto & batch = scope.load_ack_queue.front();
        for (const auto & id : batch.operations) {
            const auto & op = scope.async_ops.loadback_ops().at(id);
            const auto refs = scope.refs.release_owner(scope.tree, op.header.owner);
            sync_capacity_for_ref(scope, normalized_scope(fact), refs);
            scope.async_ops.set_loadback_state(id, HiCacheOperationState::Committed, fact.ts);
        }
        scope.load_ack_queue.pop_front();
    }
}

} // namespace markov::trace_graph::modules::hicache::model
