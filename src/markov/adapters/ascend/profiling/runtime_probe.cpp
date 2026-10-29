#include <acl/acl.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

[[noreturn]] void fail(const char * message) {
    std::fprintf(stderr, "Markov native observation failed: %s\n", message);
    std::abort();
}

std::int64_t realtime_ns() {
    timespec now{};
    if (clock_gettime(CLOCK_REALTIME, &now)) fail("clock_gettime");
    return now.tv_sec * 1'000'000'000LL + now.tv_nsec;
}

template <class Function> Function next_function(const char * name) {
    const auto address = dlsym(RTLD_NEXT, name);
    if (!address) fail(name);
    return reinterpret_cast<Function>(address);
}

class NativeWriter {
public:
    NativeWriter() {
        const auto directory = std::getenv("MARKOV_NATIVE_PROBE");
        if (directory) directory_ = directory;
    }

    ~NativeWriter() {
        if (descriptor_ >= 0) close(descriptor_);
        if (control_ != MAP_FAILED) munmap(control_, 1);
    }
    NativeWriter(const NativeWriter &) = delete;
    NativeWriter & operator=(const NativeWriter &) = delete;

    bool enabled() {
        if (directory_.empty()) return false;
        std::call_once(mapped_, &NativeWriter::map_control, this);
        return *static_cast<const volatile unsigned char *>(control_) != 0;
    }

    void write(const char * name, std::int64_t begin, std::int64_t end, void * event, void * stream, int timeout, aclError result) {
        const auto process = getpid();
        const auto duration = end - begin;
        char line[1'024];
        const auto size = std::snprintf(line,
                                        sizeof(line),
                                        "{\"name\":\"AscendCL@%s\",\"ph\":\"X\",\"pid\":%d,\"tid\":%ld,"
                                        "\"ts\":\"%lld.%03lld\",\"dur\":\"%lld.%03lld\","
                                        "\"args\":{\"return_code\":%d,\"Function-Args\":{\"event\":\"%p\",\"stream\":\"%p\",\"timeout_ms\":%d}}}\n",
                                        name,
                                        process,
                                        syscall(SYS_gettid),
                                        static_cast<long long>(begin / 1'000),
                                        static_cast<long long>(begin % 1'000),
                                        static_cast<long long>(duration / 1'000),
                                        static_cast<long long>(duration % 1'000),
                                        result,
                                        event,
                                        stream,
                                        timeout);
        if (size < 0 || static_cast<std::size_t>(size) >= sizeof(line)) fail("format native record");

        std::lock_guard lock(mutex_);
        if (process_ != process) {
            if (descriptor_ >= 0) close(descriptor_);
            const auto path = directory_ + "/runtime_" + std::to_string(process) + ".jsonl";
            descriptor_ = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (descriptor_ < 0) fail("open native record file");
            process_ = process;
        }
        std::size_t written = 0;
        while (written < static_cast<std::size_t>(size)) {
            const auto count = ::write(descriptor_, line + written, size - written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) fail("write native record");
            written += count;
        }
    }

private:
    void map_control() {
        const auto descriptor = open((directory_ + "/enabled").c_str(), O_RDONLY | O_CLOEXEC);
        if (descriptor < 0) fail("open native observation control");
        control_ = mmap(nullptr, 1, PROT_READ, MAP_SHARED, descriptor, 0);
        close(descriptor);
        if (control_ == MAP_FAILED) fail("map native observation control");
    }

    std::string directory_;
    void * control_ = MAP_FAILED;
    int descriptor_ = -1;
    pid_t process_ = 0;
    std::mutex mutex_;
    std::once_flag mapped_;
};

// SDK teardown can still call the probe. Construct its owner before loading
// the SDK, but open the capture control only on the first intercepted API.
NativeWriter process_writer;

class NativeSpan {
public:
    NativeSpan(const char * name, void * event, void * stream, int timeout = -1)
        : writer_(process_writer),
          name_(name),
          event_(event),
          stream_(stream),
          timeout_(timeout),
          enabled_(writer_.enabled()),
          begin_(enabled_ ? realtime_ns() : 0) {}

    void finish(aclError result) {
        const auto error = errno;
        if (enabled_) writer_.write(name_, begin_, realtime_ns(), event_, stream_, timeout_, result);
        errno = error;
    }

private:
    NativeWriter & writer_;
    const char * name_;
    void * event_;
    void * stream_;
    int timeout_;
    bool enabled_;
    std::int64_t begin_;
};

} // namespace

extern "C" aclError aclrtRecordEvent(aclrtEvent event, aclrtStream stream) {
    static const auto original = next_function<decltype(&aclrtRecordEvent)>("aclrtRecordEvent");
    NativeSpan span("aclrtRecordEvent", event, stream);
    const auto result = original(event, stream);
    span.finish(result);
    return result;
}

extern "C" aclError aclrtStreamWaitEvent(aclrtStream stream, aclrtEvent event) {
    static const auto original = next_function<decltype(&aclrtStreamWaitEvent)>("aclrtStreamWaitEvent");
    NativeSpan span("aclrtStreamWaitEvent", event, stream);
    const auto result = original(stream, event);
    span.finish(result);
    return result;
}

extern "C" aclError aclrtSynchronizeEvent(aclrtEvent event) {
    static const auto original = next_function<decltype(&aclrtSynchronizeEvent)>("aclrtSynchronizeEvent");
    NativeSpan span("aclrtSynchronizeEvent", event, nullptr);
    const auto result = original(event);
    span.finish(result);
    return result;
}

extern "C" aclError aclrtSynchronizeEventWithTimeout(aclrtEvent event, int32_t timeout) {
    static const auto original = next_function<decltype(&aclrtSynchronizeEventWithTimeout)>("aclrtSynchronizeEventWithTimeout");
    NativeSpan span("aclrtSynchronizeEventWithTimeout", event, nullptr, timeout);
    const auto result = original(event, timeout);
    span.finish(result);
    return result;
}

extern "C" aclError aclrtSynchronizeStream(aclrtStream stream) {
    static const auto original = next_function<decltype(&aclrtSynchronizeStream)>("aclrtSynchronizeStream");
    NativeSpan span("aclrtSynchronizeStream", nullptr, stream);
    const auto result = original(stream);
    span.finish(result);
    return result;
}

extern "C" aclError aclrtSynchronizeStreamWithTimeout(aclrtStream stream, int32_t timeout) {
    static const auto original = next_function<decltype(&aclrtSynchronizeStreamWithTimeout)>("aclrtSynchronizeStreamWithTimeout");
    NativeSpan span("aclrtSynchronizeStreamWithTimeout", nullptr, stream, timeout);
    const auto result = original(stream, timeout);
    span.finish(result);
    return result;
}
