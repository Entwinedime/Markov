#include <acl/acl.h>

#include <charconv>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

void check(aclError status) {
    if (status != ACL_SUCCESS) throw std::runtime_error("ACL call failed: " + std::to_string(status));
}

int argument(std::string_view value, int minimum) {
    int number = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || number < minimum)
        throw std::runtime_error("Invalid measurement argument: " + std::string(value));
    return number;
}

class StreamMeasurement {
public:
    explicit StreamMeasurement(int device) : device_(device) { check(aclInit(nullptr)); }
    ~StreamMeasurement() {
        if (event_) aclrtDestroyEvent(event_);
        if (stream_) aclrtDestroyStream(stream_);
        if (selected_) aclrtResetDevice(device_);
        aclFinalize();
    }
    StreamMeasurement(const StreamMeasurement &) = delete;
    StreamMeasurement & operator=(const StreamMeasurement &) = delete;

    void prepare() {
        check(aclrtSetDevice(device_));
        selected_ = true;
        check(aclrtCreateStream(&stream_));
        check(aclrtCreateEvent(&event_));
        check(aclrtRecordEvent(event_, stream_));
        synchronize();
    }

    void synchronize() { check(aclrtSynchronizeStreamWithTimeout(stream_, -1)); }

    double measure(int batch) {
        const auto begin = std::chrono::steady_clock::now();
        for (int i = 0; i < batch; ++i) synchronize();
        const auto elapsed = std::chrono::steady_clock::now() - begin;
        return std::chrono::duration<double, std::nano>(elapsed).count() / batch;
    }

private:
    int device_;
    bool selected_ = false;
    aclrtStream stream_ = nullptr;
    aclrtEvent event_ = nullptr;
};

} // namespace

int main(int argc, char ** argv) {
    try {
        if (argc != 4) throw std::runtime_error("usage: markov_stream_sync DEVICE BATCH REPEATS");
        const auto device = argument(argv[1], 0), batch = argument(argv[2], 1), repeats = argument(argv[3], 1);
        StreamMeasurement measurement(device);
        measurement.prepare();
        for (int i = 0; i < 32; ++i) measurement.synchronize();

        std::cout << "{\"implementation\":\"aclrtSynchronizeStreamWithTimeout\","
                     "\"timing_scope\":\"idle_stream_sync_interval\",\"clock\":\"steady_clock\","
                     "\"stream_state\":\"completed_event_no_pending_work\",\"timeout_ms\":-1,\"warmup\":32,\"device\":"
                  << device << ",\"batch\":" << batch << ",\"samples_ns\":[";
        for (int i = 0; i < repeats; ++i) {
            if (i) std::cout << ',';
            std::cout << measurement.measure(batch);
        }
        std::cout << "]}\n";
    }
    catch (const std::exception & error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
