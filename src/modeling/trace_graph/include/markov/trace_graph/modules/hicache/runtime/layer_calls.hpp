#pragma once
#include "markov/trace_graph/modules/hicache/layer_waits.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include <functional>

namespace markov::trace_graph::modules::hicache::runtime {

class HiCacheLayerCalls {
public:
    explicit HiCacheLayerCalls(HiCacheWriteStreamInsertions & insertions, std::string calibration = {})
        : insertions_(insertions),
          calibration_(std::move(calibration)) {}
    using Consumer = std::function<const std::vector<size_t> &(const std::string &, uint64_t)>;
    void bind(core::DagGraph & graph, uint64_t begin, uint64_t end);
    void rebind_workers(const core::DagGraph & graph, const std::map<size_t, size_t> & members);
    void advance(size_t node, const Consumer & consumer, simulation::FutureDag & future);
    [[nodiscard]] size_t prepared_calls() const { return calls_.size(); }
    [[nodiscard]] size_t active_calls() const { return active_; }
    [[nodiscard]] size_t inactive_calls() const { return inactive_; }

private:
    void bind_inactive(core::DagGraph & graph, const HiCacheLayerWaitObservation & waits, uint64_t begin, uint64_t end);
    struct Call {
        std::string pid, phase, lane;
        uint64_t batch_start_ns, layer, outside_gap_us = 0;
        std::optional<uint64_t> source_inactive_us;
        HiCacheHostExpansion plan;
        HiCacheWriteStreamPosition position;
        size_t device_lane;
        std::vector<size_t> successors, synchronizations;
        std::string insertion_issue;
    };
    std::map<size_t, Call> calls_;
    std::map<std::pair<std::string, std::string>, uint64_t> inactive_ns_, remainder_ns_;
    HiCacheWriteStreamInsertions & insertions_;
    std::string calibration_;
    size_t active_ = 0, inactive_ = 0;
};

} // namespace markov::trace_graph::modules::hicache::runtime
