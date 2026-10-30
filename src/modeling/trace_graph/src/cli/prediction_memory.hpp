#pragma once

#include <cstdint>

namespace markov::trace_graph::cli {

/** Linux container budget; allocation limits precede memory-pressure cleanup. */
class PredictionMemory {
public:
    PredictionMemory();
    void limit_source() const;
    void prepare_workers(uint64_t jobs);
    void limit_worker() const;
    bool can_start() const;
    bool pressure() const;
    uint64_t reserve_bytes() const { return reserve_; }
    uint64_t worker_bytes() const { return worker_; }

private:
    uint64_t limit_;
    uint64_t reserve_;
    uint64_t worker_ = 0;
    uint64_t source_virtual_ = 0;
};

} // namespace markov::trace_graph::cli
