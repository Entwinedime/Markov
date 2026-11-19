#include <acl/acl_rt.h>
#include <driver/ascend_hal.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "runtime_timing.hpp"

namespace {

using markov::ascend::Observation;
using markov::ascend::RuntimeSample;
using markov::ascend::host_activity;
using markov::ascend::observation;
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

struct HostTimestamp {
    std::uint64_t counter;
    std::int64_t raw_ns;
};

std::int64_t monotonic_raw_ns() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC_RAW, &now);
    return static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
}

std::uint64_t host_counter() {
    std::uint64_t counter;

    asm volatile("isb; mrs %0, cntvct_el0" : "=r"(counter) : : "memory");
    return counter;
}

HostTimestamp host_timestamp() {

    // Match the profiler's AArch64 counter for ordering; RAW remains a separately observed measurement.
    return { host_counter(), monotonic_raw_ns() };
}

class RuntimeLog {
public:
    ~RuntimeLog() { close(); }

    void append(const char * directory, const char * record, std::size_t size) {
        const auto process = getpid();
        if (process_ != process || directory_ != directory) {
            close();
            char path[4'096];
            const auto length = std::snprintf(path, sizeof(path), "%s/%d.jsonl", directory, process);
            if (length < 0 || static_cast<std::size_t>(length) >= sizeof(path)) std::abort();
            file_ = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
            if (file_ < 0) fail();
            process_ = process;
            directory_ = directory;
        }

        // One unbuffered append preserves complete concurrent records without opening the file for every API.
        if (write(file_, record, size) != static_cast<ssize_t>(size)) fail();
    }

private:
    static void fail() {
        std::perror("markov: cannot record native runtime API");
        std::abort();
    }

    void close() {
        if (file_ >= 0 && ::close(std::exchange(file_, -1)) != 0) fail();
    }

    pid_t process_ = -1;
    int file_ = -1;
    std::string directory_;
};

void append_record(const char * directory, const char * record, int size, std::size_t capacity) {
    if (size < 0 || static_cast<std::size_t>(size) >= capacity) std::abort();

    // Native teardown can log after C++ thread-local destruction; POSIX ownership permits a fresh per-thread handle.
    static const auto key = [] {
        pthread_key_t result;
        if (pthread_key_create(&result, [](void * log) { delete static_cast<RuntimeLog *>(log); }) != 0) std::abort();
        return result;
    }();
    auto * log = static_cast<RuntimeLog *>(pthread_getspecific(key));
    if (log == nullptr) {
        auto owned = std::make_unique<RuntimeLog>();
        if (pthread_setspecific(key, owned.get()) != 0) std::abort();
        log = owned.release();
    }
    log->append(directory, record, static_cast<std::size_t>(size));
}

const auto & torch_npu_code() {
    using Range = std::pair<std::uintptr_t, std::uintptr_t>;
    static const auto ranges = [] {
        std::vector<Range> result;
        dl_iterate_phdr([](dl_phdr_info * module, std::size_t, void * output) {
            if (!std::string_view(module->dlpi_name).ends_with("/libtorch_npu.so")) return 0;
            auto & code = *static_cast<std::vector<Range> *>(output);
            for (std::size_t index = 0; index < module->dlpi_phnum; ++index) {
                const auto & segment = module->dlpi_phdr[index];
                if (segment.p_type == PT_LOAD && (segment.p_flags & PF_X)) {
                    const auto begin = module->dlpi_addr + segment.p_vaddr;
                    code.emplace_back(begin, begin + segment.p_memsz);
                }
            }
            return 1;
        }, &result);
        return result;
    }();
    return ranges;
}

class QueueNotification {
public:
    explicit QueueNotification(const void * site) : directory_(directory(site)),
                                                      begin_(directory_ == nullptr ? HostTimestamp{} : host_timestamp()) {}

    void finish(const char * api, int fd, eventfd_t value) const {
        if (directory_ == nullptr) return;
        const auto end = host_timestamp();
        char record[320];
        const auto size = std::snprintf(record, sizeof(record),
                                        "{\"api\":\"%s\",\"thread\":%ld,\"fd\":%d,\"value\":%llu,\"result\":0,"
                                        "\"begin_counter\":%llu,\"end_counter\":%llu,\"begin_raw_ns\":%lld,\"end_raw_ns\":%lld}\n",
                                        api, syscall(SYS_gettid), fd, static_cast<unsigned long long>(value),
                                        static_cast<unsigned long long>(begin_.counter), static_cast<unsigned long long>(end.counter),
                                        static_cast<long long>(begin_.raw_ns), static_cast<long long>(end.raw_ns));
        append_record(directory_, record, size, sizeof(record));
    }

private:
    static const char * directory(const void * site) {
        if (observation.load(std::memory_order_relaxed) != Observation::Structure || !host_activity.load(std::memory_order_relaxed)) return nullptr;

        // Module and symbol lookup must stay off the notification path.
        const auto address = reinterpret_cast<std::uintptr_t>(site);
        const auto worker_queue = std::ranges::any_of(torch_npu_code(), [address](const auto & range) {
            return range.first <= address && address < range.second;
        });
        return worker_queue ? std::getenv("MARKOV_NATIVE_RUNTIME_DIR") : nullptr;
    }

    const char * directory_;
    HostTimestamp begin_;
};

class DriverCall {
public:
    explicit DriverCall(const char * name, bool lifecycle = false)
        : sample_(name, !lifecycle),
          directory_(record(lifecycle) ? std::getenv("MARKOV_NATIVE_RUNTIME_DIR") : nullptr),
          name_(name),
          begin_(directory_ == nullptr ? HostTimestamp{} : host_timestamp()) {
        sample_.enter();
    }

    drvError_t finish(drvError_t result, std::uint32_t device, std::uint32_t system, std::uint32_t queue,
                      std::uint32_t stream = 0, std::uint32_t tasks = 0) const {
        sample_.finish(static_cast<int>(result), tasks);
        if (directory_ == nullptr) return result;

        const auto end = host_timestamp();
        char record[512];
        const auto size = std::snprintf(record,
                                        sizeof(record),
                                        "{\"api\":\"%s\",\"thread\":%ld,\"begin_counter\":%llu,\"end_counter\":%llu,"
                                        "\"begin_raw_ns\":%lld,\"end_raw_ns\":%lld,"
                                        "\"device\":%u,\"task_system\":%u,\"queue\":%u,\"stream\":%u,\"tasks\":%u,\"service\":%lld,\"result\":%d}\n",
                                        name_,
                                        syscall(SYS_gettid),
                                        static_cast<unsigned long long>(begin_.counter),
                                        static_cast<unsigned long long>(end.counter),
                                        static_cast<long long>(begin_.raw_ns),
                                        static_cast<long long>(end.raw_ns),
                                        device,
                                        system,
                                        queue,
                                        stream,
                                        tasks,
                                        static_cast<long long>(service_scope),
                                        static_cast<int>(result));
        append_record(directory_, record, size, sizeof(record));
        return result;
    }

    drvError_t finish(drvError_t result, const void * source, const void * destination, std::size_t bytes) const {
        sample_.finish(static_cast<int>(result), 0, bytes);
        if (directory_ == nullptr) return result;

        // Driver workers need no ACL context; observing a completed transfer must not create or query one.
        const auto end = host_timestamp();
        char record[512];
        const auto size = std::snprintf(record,
                                        sizeof(record),
                                        "{\"api\":\"%s\",\"thread\":%ld,\"begin_counter\":%llu,\"end_counter\":%llu,"
                                        "\"begin_raw_ns\":%lld,\"end_raw_ns\":%lld,\"source\":\"%llx\",\"destination\":\"%llx\","
                                        "\"bytes\":%zu,\"result\":%d}\n",
                                        name_,
                                        syscall(SYS_gettid),
                                        static_cast<unsigned long long>(begin_.counter),
                                        static_cast<unsigned long long>(end.counter),
                                        static_cast<long long>(begin_.raw_ns),
                                        static_cast<long long>(end.raw_ns),
                                        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(source)),
                                        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(destination)),
                                        bytes,
                                        static_cast<int>(result));
        append_record(directory_, record, size, sizeof(record));
        return result;
    }

private:
    static bool record(bool lifecycle) {
        const auto mode = observation.load(std::memory_order_relaxed);
        return mode == Observation::Structure || (lifecycle && mode == Observation::Off);
    }

    RuntimeSample sample_;
    const char * directory_;
    const char * name_;
    HostTimestamp begin_;
};

class RuntimeCall {
public:
    RuntimeCall(const char * name, aclrtStream stream = nullptr, bool has_stream = false, std::size_t bytes = 0, int direction = -1,
                aclrtEvent event = nullptr, const void * source = nullptr, const void * destination = nullptr)
        : sample_(name),
          directory_(observation.load(std::memory_order_relaxed) == Observation::Structure ? std::getenv("MARKOV_NATIVE_RUNTIME_DIR") : nullptr),
          name_(name),
          bytes_(bytes),
          direction_(direction),
          event_(event),
          source_(source),
          destination_(destination) {
        if (directory_ == nullptr) {
            sample_.enter();
            return;
        }

        static const auto get_device = original<decltype(&aclrtGetDevice)>("aclrtGetDevice");
        static const auto get_stream = original<decltype(&aclrtStreamGetId)>("aclrtStreamGetId");
        device_status_ = get_device(&device_);
        if (has_stream) stream_status_ = get_stream(stream, &stream_);

        // Exclude observer-side identity lookup from the measured native call interval.
        begin_ = host_timestamp();
    }

    aclError finish(aclError result, aclrtEvent created = nullptr) const {
        sample_.finish(static_cast<int>(result), 0, bytes_, direction_);
        if (directory_ == nullptr) return result;

        const auto end = host_timestamp();

        // Exact buffer ranges distinguish a completed driver transfer from staging or unrelated runtime copies.
        char buffers[128]{};
        if (source_ != nullptr || destination_ != nullptr) {
            std::snprintf(buffers, sizeof(buffers), ",\"source\":\"%llx\",\"destination\":\"%llx\"",
                          static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(source_)),
                          static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(destination_)));
        }

        char record[768];
        const auto size = std::snprintf(record,
                                        sizeof(record),
                                        "{\"api\":\"%s\",\"thread\":%ld,\"begin_counter\":%llu,\"end_counter\":%llu,"
                                        "\"begin_raw_ns\":%lld,\"end_raw_ns\":%lld,"
                                        "\"device\":%d,\"device_status\":%d,\"stream\":%d,\"stream_status\":%d,"
                                        "\"bytes\":%zu,\"direction\":%d,\"event\":\"%llx\",\"result\":%d%s}\n",
                                        name_,
                                        syscall(SYS_gettid),
                                        static_cast<unsigned long long>(begin_.counter),
                                        static_cast<unsigned long long>(end.counter),
                                        static_cast<long long>(begin_.raw_ns),
                                        static_cast<long long>(end.raw_ns),
                                        device_,
                                        device_status_,
                                        stream_,
                                        stream_status_,
                                        bytes_,
                                        direction_,
                                        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(created == nullptr ? event_ : created)),
                                        result,
                                        buffers);
        append_record(directory_, record, size, sizeof(record));

        return result;
    }

private:
    RuntimeSample sample_;
    const char * directory_;
    const char * name_;
    std::size_t bytes_;
    int direction_;
    aclrtEvent event_;
    const void * source_;
    const void * destination_;

    std::int32_t device_ = -1;
    std::int32_t stream_ = -1;
    aclError device_status_ = -1;
    aclError stream_status_ = -1;
    HostTimestamp begin_{};
};

} // namespace

extern "C" int eventfd_read(int fd, eventfd_t * value) {
    static const auto call = original<decltype(&eventfd_read)>("eventfd_read");
    const QueueNotification notification(__builtin_return_address(0));
    const auto result = call(fd, value);
    if (result == 0) {
        const auto before = errno;
        notification.finish("eventfd_read", fd, *value);
        errno = before;
    }
    return result;
}

extern "C" int eventfd_write(int fd, eventfd_t value) {
    static const auto call = original<decltype(&eventfd_write)>("eventfd_write");
    const QueueNotification notification(__builtin_return_address(0));
    const auto result = call(fd, value);
    if (result == 0) {
        const auto before = errno;
        notification.finish("eventfd_write", fd, value);
        errno = before;
    }
    return result;
}

extern "C" void markov_sample_host(std::uint64_t * values) {
    const auto sample = host_timestamp();
    values[0] = sample.counter;
    values[1] = static_cast<std::uint64_t>(sample.raw_ns);
}

extern "C" void markov_sample_host_clock(std::uint64_t * values) {
    values[1] = static_cast<std::uint64_t>(monotonic_raw_ns());
    values[0] = host_counter();
    values[2] = static_cast<std::uint64_t>(monotonic_raw_ns());
}

extern "C" int markov_sample_device(std::uint32_t device, std::uint64_t * values) {
    static const auto query = driver_original<decltype(&halGetDeviceInfo)>("halGetDeviceInfo");
    auto narrowest = std::numeric_limits<std::int64_t>::max();
    for (int attempt = 0; attempt < 5; ++attempt) {
        std::int64_t native_raw, counter;
        const auto begin = monotonic_raw_ns();

        // Keep the SDK's paired queries inside one native bracket, without Python or GIL handoffs between them.
        auto status = query(device, MODULE_TYPE_SYSTEM, INFO_TYPE_MONOTONIC_RAW, &native_raw);
        if (status != 0) return static_cast<int>(status);
        status = query(device, MODULE_TYPE_SYSTEM, INFO_TYPE_SYS_COUNT, &counter);
        if (status != 0) return static_cast<int>(status);
        const auto end = monotonic_raw_ns();
        if (end - begin < narrowest) {
            narrowest = end - begin;
            values[0] = static_cast<std::uint64_t>(counter);
            values[1] = static_cast<std::uint64_t>(begin);
            values[2] = static_cast<std::uint64_t>(end);
        }
    }
    return 0;
}

extern "C" void markov_set_runtime_enabled(int enabled) {
    if (enabled && host_activity.load(std::memory_order_relaxed)) static_cast<void>(torch_npu_code());
    observation.store(enabled ? Observation::Structure : Observation::Off, std::memory_order_relaxed);
}

extern "C" std::int64_t markov_set_service_scope(std::int64_t scope) { return std::exchange(service_scope, scope); }

extern "C" void * dlsym(void * handle, const char * name) noexcept {

    // Torch-NPU resolves some SDK APIs through explicit handles; those calls must reach the same observer.
    static const auto resolve = reinterpret_cast<decltype(&dlsym)>(dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.17"));
    if (resolve == nullptr) {
        std::fputs("markov: cannot resolve the native AArch64 dlsym ABI\n", stderr);
        std::abort();
    }
    auto * native = resolve(handle, name);
    const auto observed = std::string_view(name).starts_with("aclrt") || std::string_view(name).starts_with("halSq")
                          || std::string_view(name) == "drvMemcpy" || std::string_view(name) == "halMemcpy";
    if (native == nullptr || handle == RTLD_NEXT || !observed)
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
    const RuntimeCall observation("aclrtMemcpy", nullptr, false, bytes, static_cast<int>(direction), nullptr, source, destination);
    return observation.finish(call(destination, capacity, source, bytes, direction));
}

extern "C" aclError aclrtMemcpyAsync(void * destination, std::size_t capacity, const void * source, std::size_t bytes, aclrtMemcpyKind direction,
                                     aclrtStream stream) {
    static const auto call = original<decltype(&aclrtMemcpyAsync)>("aclrtMemcpyAsync");
    const RuntimeCall observation("aclrtMemcpyAsync", stream, true, bytes, static_cast<int>(direction), nullptr, source, destination);
    return observation.finish(call(destination, capacity, source, bytes, direction, stream));
}

extern "C" DVresult drvMemcpy(DVdeviceptr destination, std::size_t capacity, DVdeviceptr source, std::size_t bytes) {
    static const auto call = driver_original<decltype(&drvMemcpy)>("drvMemcpy");
    const DriverCall observation("drvMemcpy");
    return observation.finish(call(destination, capacity, source, bytes),
                              reinterpret_cast<const void *>(source), reinterpret_cast<const void *>(destination), bytes);
}

extern "C" drvError_t halMemcpy(void * destination, std::size_t capacity, void * source, std::size_t bytes, memcpy_info * info) {
    static const auto call = driver_original<decltype(&halMemcpy)>("halMemcpy");
    const DriverCall observation("halMemcpy");
    return observation.finish(call(destination, capacity, source, bytes, info), source, destination, bytes);
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
    const DriverCall observation("halSqCqAllocate", true);
    const auto result = call(device, input, output);
    if (input->type != DRV_NORMAL_TYPE || result != DRV_ERROR_NONE) return result;

    // The normal TS mailbox begins with StreamAllocInfo.streamId, not the physical SQ number.
    return observation.finish(result, device, input->tsId, output->sqId, input->info[0]);
}

extern "C" drvError_t halSqCqFree(std::uint32_t device, halSqCqFreeInfo * input) {
    static const auto call = driver_original<decltype(&halSqCqFree)>("halSqCqFree");
    const DriverCall observation("halSqCqFree", true);
    const auto result = call(device, input);
    if (input->type != DRV_NORMAL_TYPE || result != DRV_ERROR_NONE) return result;
    return observation.finish(result, device, input->tsId, input->sqId);
}

extern "C" drvError_t halSqTaskSend(std::uint32_t device, halTaskSendInfo * input) {
    static const auto call = driver_original<decltype(&halSqTaskSend)>("halSqTaskSend");
    const DriverCall observation("halSqTaskSend");
    const auto tasks = input->sqe_num;
    const auto result = call(device, input);
    if (input->type != DRV_NORMAL_TYPE) return result;
    return observation.finish(result, device, input->tsId, input->sqId, 0, tasks);
}
