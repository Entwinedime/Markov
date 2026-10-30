#pragma once

#include <cstdint>
#include <optional>
#include <time.h>

namespace markov::adapters::ascend::sampling {

std::int64_t clock_ns(clockid_t clock);
std::optional<std::int64_t> thread_cpu_ns(std::int64_t tid);

struct Duration {
    std::int64_t wall_ns, cpu_ns;
};

class Timer {
public:
    Timer();
    Duration stop() const;
    std::int64_t begin_ns() const { return wall_; }

private:
    std::int64_t wall_, cpu_;
};

} // namespace markov::adapters::ascend::sampling
