#include <ATen/Functions.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <time.h>
#include <vector>

namespace {

std::int64_t clock_ns(clockid_t clock) {
    timespec value;
    if (clock_gettime(clock, &value)) throw std::system_error(errno, std::generic_category(), "Read allocation clock");
    return value.tv_sec * 1'000'000'000LL + value.tv_nsec;
}

struct Duration {
    std::int64_t wall_ns, cpu_ns;
};

class Timer {
public:
    Timer() : wall_(clock_ns(CLOCK_MONOTONIC_RAW)), cpu_(clock_ns(CLOCK_THREAD_CPUTIME_ID)) {}
    Duration stop() const {
        const auto cpu_end = clock_ns(CLOCK_THREAD_CPUTIME_ID);
        return { clock_ns(CLOCK_MONOTONIC_RAW) - wall_, cpu_end - cpu_ };
    }

private:
    std::int64_t wall_, cpu_;
};

} // namespace

// Python initializes torch_npu in the matching runtime. No tensors or C++
// exceptions cross this ABI; construction and destruction stay in that runtime.
extern "C" int markov_sample_tensor_allocations(int device, std::int64_t elements, int element_bytes, int strided, int warmup, int repeats, const char * path,
                                                char * error, std::size_t error_capacity) {
    try {
        if (device < 0 || device > std::numeric_limits<c10::DeviceIndex>::max() || elements <= 0 || (element_bytes != 4 && element_bytes != 8)
            || elements > std::numeric_limits<std::int64_t>::max() / element_bytes || (strided != 0 && strided != 1) || warmup < 1 || repeats < 1 || !path)
            throw std::invalid_argument("Invalid tensor-allocation sampling arguments");
        if (std::filesystem::exists(path)) throw std::invalid_argument("Allocation measurements already exist");
        const auto dtype = element_bytes == 8 ? at::kLong : at::kInt;
        const auto options = at::TensorOptions().dtype(dtype).device(c10::Device(c10::DeviceType::PrivateUse1, device));
        const auto allocate = [&] { return strided ? at::empty_strided({ elements }, { 1 }, options) : at::empty({ elements }, options); };
        const auto verify = [&](const at::Tensor & value) {
            if (value.numel() != elements || value.dim() != 1 || value.stride(0) != 1 || value.scalar_type() != dtype
                || value.device().type() != c10::DeviceType::PrivateUse1 || value.device().index() != device || !value.const_data_ptr())
                throw std::runtime_error("Allocation did not produce the declared tensor");
        };

        // Keep raw empty-clock controls; estimation subtracts their median,
        // rather than treating the observer's own time as allocator service.
        std::vector<Duration> controls, samples;
        controls.reserve(repeats);
        const auto invocations = static_cast<std::int64_t>(warmup) + repeats;
        samples.reserve(invocations);
        for (int id = 0; id < repeats; ++id) controls.push_back(Timer().stop());
        for (std::int64_t id = 0; id < invocations; ++id) {
            const Timer timer;
            const auto value = allocate();
            samples.push_back(timer.stop());

            // Metadata checks and the tensor's destruction are outside timing.
            verify(value);
        }

        std::ofstream output(path);
        if (!output) throw std::runtime_error("Cannot create allocation measurements");
        output << "invocation,warmup,wall_ns,cpu_ns,clock_wall_ns,clock_cpu_ns\n";
        for (std::size_t id = 0; id < samples.size(); ++id)
            output << static_cast<std::int64_t>(id) - warmup << ',' << (id < static_cast<std::size_t>(warmup)) << ',' << samples[id].wall_ns << ','
                   << samples[id].cpu_ns << ',' << controls[id % repeats].wall_ns << ',' << controls[id % repeats].cpu_ns << '\n';
        if (!output) throw std::runtime_error("Cannot finish allocation measurements");
        return 0;
    }
    catch (const std::exception & exception) {
        if (error && error_capacity) std::snprintf(error, error_capacity, "%s", exception.what());
        return 1;
    }
}
