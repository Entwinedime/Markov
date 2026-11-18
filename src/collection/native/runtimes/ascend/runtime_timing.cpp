#include "runtime_timing.hpp"

#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <vector>

#include <sys/syscall.h>
#include <unistd.h>

namespace markov::ascend {

std::atomic<Observation> observation{ Observation::Off };

namespace {

struct Interval {
    const char * api;
    long thread;
    int result;
    std::uint32_t tasks;
    std::size_t bytes;
    int direction;
    std::int64_t before_ns;
    std::int64_t begin_ns;
    std::int64_t end_ns;
    std::int64_t after_ns;
};

std::mutex samples_mutex;
std::vector<Interval> samples;

std::int64_t now_ns() {
    timespec time{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &time);
    return static_cast<std::int64_t>(time.tv_sec) * 1'000'000'000 + static_cast<std::int64_t>(time.tv_nsec);
}

} // namespace

RuntimeSample::RuntimeSample(const char * name, bool enabled)
    : name_(name), before_(enabled && observation.load(std::memory_order_relaxed) == Observation::Timing ? now_ns() : 0) {}

void RuntimeSample::enter() {
    if (before_) begin_ = now_ns();
}

void RuntimeSample::finish(int result, std::uint32_t tasks, std::size_t bytes, int direction) const {
    if (!begin_) return;
    const auto end = now_ns();
    thread_local const auto thread = syscall(SYS_gettid);

    // Keep observer work outside the actual API interval and defer all formatting until collection ends.
    std::lock_guard lock(samples_mutex);
    samples.push_back({ name_, thread, result, tasks, bytes, direction, before_, begin_, end, 0 });
    samples.back().after_ns = now_ns();
}

extern "C" void markov_runtime_timing_start() {
    observation.store(Observation::Timing, std::memory_order_relaxed);
}

extern "C" int markov_runtime_timing_stop(const char * path) {
    observation.store(Observation::Off, std::memory_order_relaxed);
    try {
        std::lock_guard lock(samples_mutex);
        std::ofstream output(path);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        for (const auto & sample : samples) {
            output << "{\"api\":\"" << sample.api << "\",\"thread\":" << sample.thread
                   << ",\"result\":" << sample.result << ",\"tasks\":" << sample.tasks
                   << ",\"bytes\":" << sample.bytes << ",\"direction\":" << sample.direction
                   << ",\"before_raw_ns\":" << sample.before_ns << ",\"begin_raw_ns\":" << sample.begin_ns
                   << ",\"end_raw_ns\":" << sample.end_ns << ",\"after_raw_ns\":" << sample.after_ns << "}\n";
        }
        output.close();
        samples.clear();
        return 0;
    } catch (const std::exception & error) {
        std::fprintf(stderr, "markov: %s\n", error.what());
        return -1;
    }
}

} // namespace markov::ascend
