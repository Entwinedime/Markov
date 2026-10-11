// Independent runtime service measurement. No inference, profiler or probe.
#include <acl/acl.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

// Match the runtime entrypoints intercepted by targets/sglang_hooks.cpp.
// ACL creates the stream, but its wrapper overhead is outside the measured
// synchronization boundary and already belongs to the post-sync observation.
extern "C" int rtStreamSynchronize(void * stream);
extern "C" int rtStreamSynchronizeWithTimeout(void * stream, int32_t timeout);

namespace {

void require_success(aclError status) {
    if (status != ACL_SUCCESS) throw std::runtime_error("ACL calibration failed: " + std::to_string(status));
}

class Runtime {
public:
    explicit Runtime(int device) : device_(device) {
        require_success(aclInit(nullptr));
        try {
            require_success(aclrtSetDevice(device_));
        }
        catch (...) {
            aclFinalize();
            throw;
        }
    }

    ~Runtime() {
        aclrtResetDevice(device_);
        aclFinalize();
    }

    Runtime(const Runtime &) = delete;
    Runtime & operator=(const Runtime &) = delete;

private:
    int device_;
};

class Stream {
public:
    Stream() { require_success(aclrtCreateStream(&value)); }
    ~Stream() { aclrtDestroyStream(value); }

    Stream(const Stream &) = delete;
    Stream & operator=(const Stream &) = delete;

    aclrtStream value{};
};

class Event {
public:
    Event() { require_success(aclrtCreateEvent(&value)); }
    ~Event() { aclrtDestroyEvent(value); }

    Event(const Event &) = delete;
    Event & operator=(const Event &) = delete;

    aclrtEvent value{};
};

template <typename Synchronize> double measure(Synchronize synchronize, int batch) {
    const auto start = std::chrono::steady_clock::now();
    for (int call = 0; call < batch; ++call) require_success(synchronize());
    const auto end = std::chrono::steady_clock::now();

    return std::chrono::duration<double, std::micro>(end - start).count() / batch;
}

} // namespace

int main(int argc, char ** argv) {
    try {
        if (argc != 4) throw std::invalid_argument("usage: idle_stream_sync DEVICE BATCH REPEATS");
        const int device = std::stoi(argv[1]);
        const int batch = std::stoi(argv[2]);
        const int repeats = std::stoi(argv[3]);
        if (device < 0 || batch <= 0 || repeats <= 0) throw std::invalid_argument("invalid measurement domain");

        Runtime runtime(device);
        Stream stream;
        Event completed;
        const auto ordinary = [&] { return rtStreamSynchronize(stream.value); };
        const auto timeout = [&] { return rtStreamSynchronizeWithTimeout(stream.value, -1); };
        // Establish a used but idle stream, rather than measuring only the
        // virgin-stream shortcut. This happens outside every timing interval.
        require_success(aclrtRecordEvent(completed.value, stream.value));
        require_success(ordinary());

        // Both API variants are warmed independently; the timeout variant
        // waits indefinitely. No device work is submitted during measurement.
        measure(ordinary, batch);
        measure(timeout, batch);

        std::cout << "{\"device\":" << device << ",\"batch\":" << batch << ",\"samples\":[";
        for (int repeat = 0; repeat < repeats; ++repeat) {
            double ordinary_us, timeout_us;
            if (repeat % 2 == 0) {
                ordinary_us = measure(ordinary, batch);
                timeout_us = measure(timeout, batch);
            }
            else {
                timeout_us = measure(timeout, batch);
                ordinary_us = measure(ordinary, batch);
            }

            if (repeat != 0) std::cout << ',';
            std::cout << "{\"repeat\":" << repeat << ",\"rtStreamSynchronize_us\":" << ordinary_us << ",\"rtStreamSynchronizeWithTimeout_us\":" << timeout_us
                      << '}';
        }
        std::cout << "]}\n";
        return 0;
    }
    catch (const std::exception & error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
