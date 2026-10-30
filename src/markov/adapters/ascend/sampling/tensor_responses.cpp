#include "markov/adapters/ascend/sampling/clock.hpp"

#include <ATen/DeviceAccelerator.h>
#include <ATen/Functions.h>
#include <ATen/record_function.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using namespace markov::adapters::ascend::sampling;
using ThreadClocks = std::map<std::int64_t, std::int64_t>;

ThreadClocks thread_clocks() {
    ThreadClocks result;
    for (const auto & entry : std::filesystem::directory_iterator("/proc/self/task")) {
        const auto tid = std::stoll(entry.path().filename().string());
        if (const auto cpu = thread_cpu_ns(tid)) result.emplace(tid, *cpu);
    }
    return result;
}

class NativeTensorSampler {
public:
    NativeTensorSampler(int device, const std::int64_t * sizes, std::size_t count, int element_bytes, bool concatenate, int warmup, int case_id, bool markers,
                        const char * path)
        : device_(device),
          concatenate_(concatenate),
          warmup_(warmup),
          case_id_(case_id),
          markers_(markers) {
        const auto dtype = element_bytes == 8 ? at::kLong : at::kInt;
        const auto options = at::TensorOptions().dtype(dtype).device(c10::Device(c10::DeviceType::PrivateUse1, device_));
        std::vector<at::Tensor> expected;
        for (std::size_t id = 0; id < count; ++id) {
            if (sizes[id] <= 0) throw std::invalid_argument("Tensor input sizes must be positive");
            expected.push_back(at::arange(sizes[id], at::TensorOptions().dtype(dtype)));
            inputs_.push_back(expected.back().to(options));
        }
        values_ = at::cat(expected);
        at::accelerator::synchronizeDevice(device_);
        calls_.open(path);
        threads_.open(std::string(path) + ".threads.csv");
        if (!calls_ || !threads_) throw std::runtime_error("Cannot create native tensor measurements");
        calls_ << "ordinal,warmup,control,caller_wall_ns,caller_thread_cpu_ns,completion_observed_ns,observation_wall_ns,pid\n";
        threads_ << "ordinal,control,tid,before_ns,after_ns\n";
    }

    void collect(int repeats) {
        for (int ordinal = 0; ordinal < repeats; ++ordinal) sample(ordinal, true);
        for (int ordinal = 0; ordinal < warmup_ + repeats; ++ordinal) sample(ordinal, false);
        calls_.close();
        threads_.close();
        if (!calls_ || !threads_) throw std::runtime_error("Cannot finish native tensor measurements");
    }

private:
    void sample(int ordinal, bool control) {
        at::accelerator::synchronizeDevice(device_);
        const auto observation_begin = clock_ns(CLOCK_MONOTONIC_RAW);
        const auto before = thread_clocks();
        std::optional<at::RecordFunction> marker;
        const auto name = "markov.tensor:" + std::to_string(case_id_) + ":" + std::to_string(ordinal);
        if (markers_ && !control) {
            marker.emplace(at::RecordScope::USER_SCOPE);
            marker->before(name.c_str());
        }
        at::Tensor value;
        const Timer timer;
        if (!control) value = concatenate_ ? at::cat(inputs_) : inputs_.front().clone();
        const auto duration = timer.stop();
        marker.reset();
        at::accelerator::synchronizeDevice(device_);
        const auto completed = clock_ns(CLOCK_MONOTONIC_RAW) - timer.begin_ns();
        const auto after = thread_clocks();
        const auto observed = clock_ns(CLOCK_MONOTONIC_RAW) - observation_begin;
        calls_ << ordinal << ',' << (!control && ordinal < warmup_) << ',' << control << ',' << duration.wall_ns << ',' << duration.cpu_ns << ',' << completed
               << ',' << observed << ',' << getpid() << '\n';
        for (const auto & [tid, cpu] : before) {
            const auto end = after.find(tid);
            if (end != after.end()) threads_ << ordinal << ',' << control << ',' << tid << ',' << cpu << ',' << end->second << '\n';
        }
        if (!control && !at::equal(value.cpu(), values_)) throw std::runtime_error("Native tensor operation returned incorrect values");
    }

    int device_;
    bool concatenate_;
    int warmup_, case_id_;
    bool markers_;
    std::vector<at::Tensor> inputs_;
    at::Tensor values_;
    std::ofstream calls_, threads_;
};

} // namespace

// The bridge records actual C++ API time. Python initialization, tensor
// validation, thread-clock reads and synchronization stay outside that timer.
extern "C" int markov_sample_tensor_responses(int device, const std::int64_t * sizes, std::size_t count, int element_bytes, int concatenate, int warmup,
                                              int repeats, int case_id, int markers, const char * path, char * error, std::size_t error_capacity) {
    try {
        if (device < 0 || device > std::numeric_limits<c10::DeviceIndex>::max() || !sizes || !count || (element_bytes != 4 && element_bytes != 8)
            || (concatenate != 0 && concatenate != 1) || (!concatenate && count != 1) || warmup < 0 || repeats < 1 || case_id < 0
            || static_cast<std::int64_t>(warmup) + repeats > std::numeric_limits<int>::max() || (markers != 0 && markers != 1) || !path)
            throw std::invalid_argument("Invalid native tensor sampling arguments");
        const auto threads_path = std::string(path) + ".threads.csv";
        if (std::filesystem::exists(path) || std::filesystem::exists(threads_path)) throw std::invalid_argument("Tensor measurements already exist");
        NativeTensorSampler sampler(device, sizes, count, element_bytes, concatenate, warmup, case_id, markers, path);
        sampler.collect(repeats);
        return 0;
    }
    catch (const std::exception & exception) {
        if (error && error_capacity) std::snprintf(error, error_capacity, "%s", exception.what());
        return 1;
    }
}
