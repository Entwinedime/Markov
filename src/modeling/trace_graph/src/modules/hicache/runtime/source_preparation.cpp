#include "modules/hicache/runtime/source_preparation.hpp"

#include "markov/trace_graph/source_dag/decode_allocation.hpp"

#include <utility>

namespace markov::trace_graph::modules::hicache::runtime {
namespace {

std::shared_ptr<const model::HiCacheSourceInput> execution_input(const core::DagGraph & graph, uint64_t begin_us, uint64_t end_us) {
    auto input = std::make_shared<model::HiCacheSourceInput>(model::observe_hicache_source_input(graph));
    input->decode_allocations = source_dag::observe_decode_allocations(graph, input->formal, input->phases, begin_us, end_us);
    return input;
}

} // namespace

HiCacheSourcePreparation::HiCacheSourcePreparation(const core::DagGraph & graph, uint64_t begin_us, uint64_t end_us)
    : index(graph),
      queues(simulation::detail::discover_cpu_task_queues(graph)),
      input(execution_input(graph, begin_us, end_us)),
      phase_calls(adapters::sglang::observe_phase_calls(index, queues, input->phases)),
      allocator(adapters::sglang::observe_allocator_preparations(graph)),
      allocator_regions(adapters::sglang::observe_allocator_regions(index, queues)) {}

} // namespace markov::trace_graph::modules::hicache::runtime
