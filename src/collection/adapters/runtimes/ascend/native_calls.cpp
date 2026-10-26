#include <ATen/core/TensorBody.h>
#include <acl/acl_rt.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

std::atomic_bool runtime_enabled{ true };

template <class Function> Function original(const char * name) {
    auto * symbol = dlsym(RTLD_NEXT, name);
    if (symbol == nullptr) {
        std::fprintf(stderr, "markov: cannot resolve native API %s: %s\n", name, dlerror());
        std::abort();
    }

    return reinterpret_cast<Function>(symbol);
}

std::int64_t raw_time_ns() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC_RAW, &now);
    return static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
}

class RuntimeCall {
public:
    RuntimeCall(const char * name, aclrtStream stream, bool has_stream, std::size_t bytes = 0, int direction = -1)
        : directory_(runtime_enabled.load(std::memory_order_relaxed) ? std::getenv("MARKOV_NATIVE_RUNTIME_DIR") : nullptr),
          name_(name),
          bytes_(bytes),
          direction_(direction) {
        if (directory_ == nullptr) return;

        static const auto get_device = original<decltype(&aclrtGetDevice)>("aclrtGetDevice");
        static const auto get_stream = original<decltype(&aclrtStreamGetId)>("aclrtStreamGetId");
        device_status_ = get_device(&device_);
        if (has_stream) stream_status_ = get_stream(stream, &stream_);

        // Exclude observer-side identity lookup from the measured native call interval.
        begin_ns_ = raw_time_ns();
    }

    aclError finish(aclError result) const {
        if (directory_ == nullptr) return result;

        const auto end_ns = raw_time_ns();

        char path[4'096];
        const auto path_size = std::snprintf(path, sizeof(path), "%s/%d.jsonl", directory_, getpid());
        char record[512];
        const auto size = std::snprintf(record,
                                        sizeof(record),
                                        "{\"api\":\"%s\",\"thread\":%ld,\"begin_raw_ns\":%lld,\"end_raw_ns\":%lld,"
                                        "\"device\":%d,\"device_status\":%d,\"stream\":%d,\"stream_status\":%d,"
                                        "\"bytes\":%zu,\"direction\":%d,\"result\":%d}\n",
                                        name_,
                                        syscall(SYS_gettid),
                                        static_cast<long long>(begin_ns_),
                                        static_cast<long long>(end_ns),
                                        device_,
                                        device_status_,
                                        stream_,
                                        stream_status_,
                                        bytes_,
                                        direction_,
                                        result);
        if (path_size < 0 || static_cast<std::size_t>(path_size) >= sizeof(path) || size < 0 || static_cast<std::size_t>(size) >= sizeof(record)) std::abort();

        // One append per call avoids inherited buffered state and locks across native worker forks.
        const auto file = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (file < 0 || write(file, record, static_cast<std::size_t>(size)) != size || close(file) != 0) {
            std::perror("markov: cannot record native runtime API");
            std::abort();
        }

        return result;
    }

private:
    const char * directory_;
    const char * name_;
    std::size_t bytes_;
    int direction_;

    std::int32_t device_ = -1;
    std::int32_t stream_ = -1;
    aclError device_status_ = -1;
    aclError stream_status_ = -1;
    std::int64_t begin_ns_ = 0;
};

} // namespace

extern "C" void markov_set_runtime_enabled(int enabled) { runtime_enabled.store(enabled != 0, std::memory_order_relaxed); }

extern "C" aclError aclrtMemcpy(void * destination, std::size_t capacity, const void * source, std::size_t bytes, aclrtMemcpyKind direction) {
    static const auto call = original<decltype(&aclrtMemcpy)>("aclrtMemcpy");
    const RuntimeCall observation("aclrtMemcpy", nullptr, false, bytes, static_cast<int>(direction));
    return observation.finish(call(destination, capacity, source, bytes, direction));
}

extern "C" aclError aclrtMemcpyAsync(void * destination, std::size_t capacity, const void * source, std::size_t bytes, aclrtMemcpyKind direction,
                                     aclrtStream stream) {
    static const auto call = original<decltype(&aclrtMemcpyAsync)>("aclrtMemcpyAsync");
    const RuntimeCall observation("aclrtMemcpyAsync", stream, true, bytes, static_cast<int>(direction));
    return observation.finish(call(destination, capacity, source, bytes, direction, stream));
}

extern "C" aclError aclrtSynchronizeStream(aclrtStream stream) {
    static const auto call = original<decltype(&aclrtSynchronizeStream)>("aclrtSynchronizeStream");
    const RuntimeCall observation("aclrtSynchronizeStream", stream, true);
    return observation.finish(call(stream));
}

extern "C" int markov_reserve_workspace(void * allocator, std::uint64_t bytes, aclrtStream stream) {
    try {

        // The caller resolves its installed allocator; the temporary Tensor releases its use, retaining the native cache.
        const auto allocate = reinterpret_cast<at::Tensor (*)(std::uint64_t, aclrtStream)>(allocator);
        const auto workspace = allocate(bytes, stream);
        return 0;
    }
    catch (const std::exception & error) {
        std::fprintf(stderr, "markov: workspace warmup failed: %s\n", error.what());
        return -1;
    }
}
