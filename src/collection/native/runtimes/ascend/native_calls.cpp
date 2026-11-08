#include <acl/acl_rt.h>
#include <driver/ascend_hal.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string_view>
#include <utility>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

std::atomic_bool runtime_enabled{ false };
thread_local std::int64_t service_scope = -1;

template <class Function> Function original(const char * name) {
    auto * symbol = dlsym(RTLD_NEXT, name);
    if (symbol == nullptr) {
        std::fprintf(stderr, "markov: cannot resolve native API %s: %s\n", name, dlerror());
        std::abort();
    }

    return reinterpret_cast<Function>(symbol);
}

template <class Function> Function driver_original(const char * name) {

    // MemFabric loads the driver locally; RTLD_NEXT cannot reliably resolve that handle.
    static auto * handle = dlopen("libascend_hal.so", RTLD_NOW | RTLD_NODELETE);
    static const auto resolve = reinterpret_cast<decltype(&dlsym)>(dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.17"));
    auto * symbol = handle == nullptr || resolve == nullptr ? nullptr : resolve(handle, name);
    if (symbol == nullptr) {
        std::fprintf(stderr, "markov: cannot resolve native driver API %s: %s\n", name, dlerror());
        std::abort();
    }
    return reinterpret_cast<Function>(symbol);
}

std::int64_t raw_time_ns() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC_RAW, &now);
    return static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
}

void append_record(const char * directory, const char * record, int size, std::size_t capacity) {
    char path[4'096];
    const auto path_size = std::snprintf(path, sizeof(path), "%s/%d.jsonl", directory, getpid());
    if (path_size < 0 || static_cast<std::size_t>(path_size) >= sizeof(path) || size < 0 || static_cast<std::size_t>(size) >= capacity) std::abort();

    // One append avoids inherited buffered state and locks across native worker forks.
    const auto file = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (file < 0 || write(file, record, static_cast<std::size_t>(size)) != size || close(file) != 0) {
        std::perror("markov: cannot record native runtime API");
        std::abort();
    }
}

class DriverQueueCall {
public:
    explicit DriverQueueCall(const char * name, bool lifecycle = false)
        : directory_(lifecycle || runtime_enabled.load(std::memory_order_relaxed) ? std::getenv("MARKOV_NATIVE_RUNTIME_DIR") : nullptr),
          name_(name),
          begin_ns_(directory_ == nullptr ? 0 : raw_time_ns()) {}

    drvError_t finish(drvError_t result, std::uint32_t device, std::uint32_t system, std::uint32_t queue, std::uint32_t stream = 0) const {
        if (directory_ == nullptr) return result;

        const auto end_ns = raw_time_ns();
        char record[512];
        const auto size = std::snprintf(record,
                                        sizeof(record),
                                        "{\"api\":\"%s\",\"thread\":%ld,\"begin_raw_ns\":%lld,\"end_raw_ns\":%lld,"
                                        "\"device\":%u,\"task_system\":%u,\"queue\":%u,\"stream\":%u,\"service\":%lld,\"result\":%d}\n",
                                        name_,
                                        syscall(SYS_gettid),
                                        static_cast<long long>(begin_ns_),
                                        static_cast<long long>(end_ns),
                                        device,
                                        system,
                                        queue,
                                        stream,
                                        static_cast<long long>(service_scope),
                                        static_cast<int>(result));
        append_record(directory_, record, size, sizeof(record));
        return result;
    }

private:
    const char * directory_;
    const char * name_;
    std::int64_t begin_ns_;
};

class RuntimeCall {
public:
    RuntimeCall(const char * name, aclrtStream stream = nullptr, bool has_stream = false, std::size_t bytes = 0, int direction = -1, aclrtEvent event = nullptr)
        : directory_(runtime_enabled.load(std::memory_order_relaxed) ? std::getenv("MARKOV_NATIVE_RUNTIME_DIR") : nullptr),
          name_(name),
          bytes_(bytes),
          direction_(direction),
          event_(event) {
        if (directory_ == nullptr) return;

        static const auto get_device = original<decltype(&aclrtGetDevice)>("aclrtGetDevice");
        static const auto get_stream = original<decltype(&aclrtStreamGetId)>("aclrtStreamGetId");
        device_status_ = get_device(&device_);
        if (has_stream) stream_status_ = get_stream(stream, &stream_);

        // Exclude observer-side identity lookup from the measured native call interval.
        begin_ns_ = raw_time_ns();
    }

    aclError finish(aclError result, aclrtEvent created = nullptr) const {
        if (directory_ == nullptr) return result;

        const auto end_ns = raw_time_ns();

        char record[512];
        const auto size = std::snprintf(record,
                                        sizeof(record),
                                        "{\"api\":\"%s\",\"thread\":%ld,\"begin_raw_ns\":%lld,\"end_raw_ns\":%lld,"
                                        "\"device\":%d,\"device_status\":%d,\"stream\":%d,\"stream_status\":%d,"
                                        "\"bytes\":%zu,\"direction\":%d,\"event\":\"%llx\",\"result\":%d}\n",
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
                                        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(created == nullptr ? event_ : created)),
                                        result);
        append_record(directory_, record, size, sizeof(record));

        return result;
    }

private:
    const char * directory_;
    const char * name_;
    std::size_t bytes_;
    int direction_;
    aclrtEvent event_;

    std::int32_t device_ = -1;
    std::int32_t stream_ = -1;
    aclError device_status_ = -1;
    aclError stream_status_ = -1;
    std::int64_t begin_ns_ = 0;
};

} // namespace

extern "C" void markov_set_runtime_enabled(int enabled) { runtime_enabled.store(enabled != 0, std::memory_order_relaxed); }

extern "C" std::int64_t markov_set_service_scope(std::int64_t scope) { return std::exchange(service_scope, scope); }

extern "C" void * dlsym(void * handle, const char * name) noexcept {

    // Torch-NPU resolves some SDK APIs through explicit handles; those calls must reach the same observer.
    static const auto resolve = reinterpret_cast<decltype(&dlsym)>(dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.17"));
    if (resolve == nullptr) {
        std::fputs("markov: cannot resolve the native AArch64 dlsym ABI\n", stderr);
        std::abort();
    }
    auto * native = resolve(handle, name);
    if (native == nullptr || handle == RTLD_NEXT || (!std::string_view(name).starts_with("aclrt") && !std::string_view(name).starts_with("halSq")))
        return native;

    auto * candidate = resolve(RTLD_DEFAULT, name);
    if (candidate == nullptr) {
        dlerror();
        return native;
    }
    Dl_info owner{};
    Dl_info target{};
    if (dladdr(reinterpret_cast<void *>(&markov_set_runtime_enabled), &owner) != 0 && dladdr(candidate, &target) != 0 && owner.dli_fbase == target.dli_fbase)
        return candidate;
    return native;
}

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

extern "C" aclError aclrtSynchronizeStreamWithTimeout(aclrtStream stream, std::int32_t timeout) {
    static const auto call = original<decltype(&aclrtSynchronizeStreamWithTimeout)>("aclrtSynchronizeStreamWithTimeout");
    const RuntimeCall observation("aclrtSynchronizeStreamWithTimeout", stream, true);
    return observation.finish(call(stream, timeout));
}

extern "C" aclError aclrtSynchronizeDeviceWithTimeout(std::int32_t timeout) {
    static const auto call = original<decltype(&aclrtSynchronizeDeviceWithTimeout)>("aclrtSynchronizeDeviceWithTimeout");
    const RuntimeCall observation("aclrtSynchronizeDeviceWithTimeout");
    return observation.finish(call(timeout));
}

extern "C" aclError aclrtCreateEvent(aclrtEvent * event) {
    static const auto call = original<decltype(&aclrtCreateEvent)>("aclrtCreateEvent");
    const RuntimeCall observation("aclrtCreateEvent");
    const auto result = call(event);
    return observation.finish(result, result == ACL_SUCCESS ? *event : nullptr);
}

extern "C" aclError aclrtCreateEventWithFlag(aclrtEvent * event, std::uint32_t flag) {
    static const auto call = original<decltype(&aclrtCreateEventWithFlag)>("aclrtCreateEventWithFlag");
    const RuntimeCall observation("aclrtCreateEventWithFlag");
    const auto result = call(event, flag);
    return observation.finish(result, result == ACL_SUCCESS ? *event : nullptr);
}

extern "C" aclError aclrtCreateEventExWithFlag(aclrtEvent * event, std::uint32_t flag) {
    static const auto call = original<decltype(&aclrtCreateEventExWithFlag)>("aclrtCreateEventExWithFlag");
    const RuntimeCall observation("aclrtCreateEventExWithFlag");
    const auto result = call(event, flag);
    return observation.finish(result, result == ACL_SUCCESS ? *event : nullptr);
}

extern "C" aclError aclrtDestroyEvent(aclrtEvent event) {
    static const auto call = original<decltype(&aclrtDestroyEvent)>("aclrtDestroyEvent");
    const RuntimeCall observation("aclrtDestroyEvent", nullptr, false, 0, -1, event);
    return observation.finish(call(event));
}

extern "C" aclError aclrtRecordEvent(aclrtEvent event, aclrtStream stream) {
    static const auto call = original<decltype(&aclrtRecordEvent)>("aclrtRecordEvent");
    const RuntimeCall observation("aclrtRecordEvent", stream, true, 0, -1, event);
    return observation.finish(call(event, stream));
}

extern "C" aclError aclrtStreamWaitEvent(aclrtStream stream, aclrtEvent event) {
    static const auto call = original<decltype(&aclrtStreamWaitEvent)>("aclrtStreamWaitEvent");
    const RuntimeCall observation("aclrtStreamWaitEvent", stream, true, 0, -1, event);
    return observation.finish(call(stream, event));
}

extern "C" aclError aclrtStreamWaitEventWithTimeout(aclrtStream stream, aclrtEvent event, std::int32_t timeout) {
    static const auto call = original<decltype(&aclrtStreamWaitEventWithTimeout)>("aclrtStreamWaitEventWithTimeout");
    const RuntimeCall observation("aclrtStreamWaitEventWithTimeout", stream, true, 0, -1, event);
    return observation.finish(call(stream, event, timeout));
}

extern "C" aclError aclrtSynchronizeEvent(aclrtEvent event) {
    static const auto call = original<decltype(&aclrtSynchronizeEvent)>("aclrtSynchronizeEvent");
    const RuntimeCall observation("aclrtSynchronizeEvent", nullptr, false, 0, -1, event);
    return observation.finish(call(event));
}

extern "C" aclError aclrtSynchronizeEventWithTimeout(aclrtEvent event, std::int32_t timeout) {
    static const auto call = original<decltype(&aclrtSynchronizeEventWithTimeout)>("aclrtSynchronizeEventWithTimeout");
    const RuntimeCall observation("aclrtSynchronizeEventWithTimeout", nullptr, false, 0, -1, event);
    return observation.finish(call(event, timeout));
}

extern "C" drvError_t halSqCqAllocate(std::uint32_t device, halSqCqInputInfo * input, halSqCqOutputInfo * output) {
    static const auto call = driver_original<decltype(&halSqCqAllocate)>("halSqCqAllocate");

    // Queue identity is established during warmup; retain its lifecycle outside the observation window.
    const DriverQueueCall observation("halSqCqAllocate", true);
    const auto result = call(device, input, output);
    if (input->type != DRV_NORMAL_TYPE || result != DRV_ERROR_NONE) return result;

    // The normal TS mailbox begins with StreamAllocInfo.streamId, not the physical SQ number.
    return observation.finish(result, device, input->tsId, output->sqId, input->info[0]);
}

extern "C" drvError_t halSqCqFree(std::uint32_t device, halSqCqFreeInfo * input) {
    static const auto call = driver_original<decltype(&halSqCqFree)>("halSqCqFree");
    const DriverQueueCall observation("halSqCqFree", true);
    const auto result = call(device, input);
    if (input->type != DRV_NORMAL_TYPE || result != DRV_ERROR_NONE) return result;
    return observation.finish(result, device, input->tsId, input->sqId);
}

extern "C" drvError_t halSqTaskSend(std::uint32_t device, halTaskSendInfo * input) {
    static const auto call = driver_original<decltype(&halSqTaskSend)>("halSqTaskSend");
    const DriverQueueCall observation("halSqTaskSend");
    const auto result = call(device, input);
    if (input->type != DRV_NORMAL_TYPE) return result;
    return observation.finish(result, device, input->tsId, input->sqId);
}
