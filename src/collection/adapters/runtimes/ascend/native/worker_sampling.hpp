#pragma once

#include <cstdint>

// Shared scalar ABI for thread-local wait counters, also read by Python.
struct CpuCounters {
    std::int64_t wait_cpu_ns = 0;
    std::int64_t probe_cpu_ns = 0;
    std::int64_t wait_wall_ns = 0;
    std::int64_t last_sync_start_ns = 0;
    std::int64_t last_sync_end_ns = 0;
    std::int64_t last_sync_cpu_end_ns = 0;
    std::int64_t last_sync_probe_cpu_ns = 0;
    std::int64_t last_sync_result = 0;
};

extern "C" void markov_native_read_cpu_counters(CpuCounters * counters);

// Internal mutable access shares the same calling-thread counters with runtime
// wait observation. Python continues to read a copied scalar ABI.
CpuCounters & markov_native_cpu_counters();

// Submission points are recorded only while a measured queue worker is active.
// The observations retain thread clocks, not qualified service estimates.
class WorkerSubmission {
public:
    explicit WorkerSubmission(const char * api);
    ~WorkerSubmission();

    WorkerSubmission(const WorkerSubmission &) = delete;
    WorkerSubmission & operator=(const WorkerSubmission &) = delete;

private:
    const char * api_;
};
