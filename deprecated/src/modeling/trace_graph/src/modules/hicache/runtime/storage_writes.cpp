#include "markov/trace_graph/modules/hicache/runtime/storage_writes.hpp"

#include <optional>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {
namespace {

struct StorageWriteProgram {
    struct Batch {
        size_t service, returned;
    };
    // All nodes execute in this order on one storage lane. The trailing
    // return is an observation, not another physical transaction.
    std::vector<core::DagSyntheticNodeSpec> nodes;
    std::vector<Batch> batches;
};

StorageWriteProgram generate_storage_write(const model::HiCacheStorageWrite & write) {
    StorageWriteProgram result;
    for (const auto & [pages, existing_pages] : write.batches) {
        const auto service = result.nodes.size();
        result.nodes.push_back({
            .name = "hicache_storage_write_batch",
            .category = "hicache_patch",
            .is_cpu = false,
            .lane_key = write.resource_lane,
            .attrs = { { "operation_id", write.header.operation_id },
                      { "cache_scope", write.header.cache_scope },
                      { "request_id", write.header.request_id },
                      { "page_count", std::to_string(pages) },
                      { "existing_page_count", std::to_string(existing_pages) } }
        });
        const auto returned = result.nodes.size();
        result.nodes.push_back({ .name = "hicache_storage_batch_return", .category = "hicache_patch", .is_cpu = false, .lane_key = write.resource_lane });
        result.batches.push_back({ service, returned });
    }

    result.nodes.push_back({
        .name = "hicache_storage_write_return",
        .category = "hicache_patch",
        .is_cpu = false,
        .lane_key = write.resource_lane,
        .attrs = { { "operation_id", write.header.operation_id }, { "cache_scope", write.header.cache_scope } }
    });
    return result;
}

} // namespace

void HiCacheStorageWrites::advance(size_t node, uint64_t absolute_time_us, simulation::FutureDag & future) {
    if (const auto event = batch_events_.find(node); event != batch_events_.end()) {
        const auto & batch = event->second;
        if (batch.completed) state_.complete_storage_batch(batch.scope, batch.operation, batch.batch, absolute_time_us);
        else state_.begin_storage_batch(batch.scope, batch.operation, batch.batch, absolute_time_us);
        batch_events_.erase(event);
    }

    if (const auto returned = returns_.find(node); returned != returns_.end()) {
        const auto & [scope, operation] = returned->second;
        state_.complete_storage_write(scope, operation, absolute_time_us);
        returns_.erase(returned);
    }

    for (const auto & write : state_.take_storage_submissions()) {
        auto program = generate_storage_write(write);
        for (size_t index = 0; index < write.batches.size(); ++index) {
            const auto & [pages, existing_pages] = write.batches[index];
            auto & service = program.nodes[program.batches[index].service];
            service.duration = completion_.duration("write_host_to_storage", page_bytes_, pages, existing_pages);
            service.attrs["cost_binding_source"] = "hicache_payload_model";
        }

        // Resolve all required costs before emitting this write. FutureDag
        // supplies the dependency on the current publication callback;
        // predecessor additionally preserves the storage resource's FIFO.
        std::optional<size_t> predecessor;
        if (const auto tail = lane_tail_.find(write.resource_lane); tail != lane_tail_.end()) predecessor = tail->second;
        std::vector<size_t> nodes;
        nodes.reserve(program.nodes.size());
        for (const auto & spec : program.nodes) {
            const auto emitted = future.append(spec);
            if (predecessor) future.depend(*predecessor, emitted);
            predecessor = emitted;
            nodes.push_back(emitted);
        }

        for (size_t index = 0; index < program.batches.size(); ++index) {
            const auto & batch = program.batches[index];
            batch_events_.emplace(nodes[batch.service], BatchEvent{ write.header.cache_scope, write.header.operation_id, index, false });
            batch_events_.emplace(nodes[batch.returned], BatchEvent{ write.header.cache_scope, write.header.operation_id, index, true });
        }
        const auto returned = nodes.back();
        lane_tail_[write.resource_lane] = returned;
        returns_.emplace(returned, std::pair{ write.header.cache_scope, write.header.operation_id });
    }
}

} // namespace markov::trace_graph::modules::hicache::runtime
