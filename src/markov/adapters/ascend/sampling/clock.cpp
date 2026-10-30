#include "markov/adapters/ascend/sampling/clock.hpp"

#include <cerrno>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace markov::adapters::ascend::sampling {

std::int64_t clock_ns(clockid_t clock) {
    timespec value;
    if (clock_gettime(clock, &value)) throw std::system_error(errno, std::generic_category(), "Read sampling clock");
    return value.tv_sec * 1'000'000'000LL + value.tv_nsec;
}

std::optional<std::int64_t> thread_cpu_ns(std::int64_t tid) {
    // Linux's per-thread CPU clock ID is -8 * tid - 2. No pthread_t or SDK
    // callback pointer crosses the sampler boundary.
    if (tid <= 0 || tid > (-(static_cast<std::int64_t>(std::numeric_limits<clockid_t>::min())) - 2) / 8)
        throw std::invalid_argument("Thread ID cannot be represented as a CPU clock");
    timespec value;
    if (clock_gettime(static_cast<clockid_t>(-8 * tid - 2), &value)) {
        if (errno == EINVAL || errno == ESRCH) return std::nullopt;
        throw std::system_error(errno, std::generic_category(), "Read worker CPU clock");
    }
    return value.tv_sec * 1'000'000'000LL + value.tv_nsec;
}

Timer::Timer() : wall_(clock_ns(CLOCK_MONOTONIC_RAW)), cpu_(clock_ns(CLOCK_THREAD_CPUTIME_ID)) {}

Duration Timer::stop() const {
    const auto cpu_end = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    return { clock_ns(CLOCK_MONOTONIC_RAW) - wall_, cpu_end - cpu_ };
}

} // namespace markov::adapters::ascend::sampling
