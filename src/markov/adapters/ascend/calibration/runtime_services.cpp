#include <acl/acl.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

void check(int status) {
    if (status != 0) throw std::runtime_error("Ascend service call failed: " + std::to_string(status));
}

int argument(std::string_view value, int minimum) {
    int number = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || number < minimum)
        throw std::runtime_error("Invalid measurement argument: " + std::string(value));
    return number;
}

class RuntimeServices {
public:
    explicit RuntimeServices(int device) : device_(device) { check(aclInit(nullptr)); }
    ~RuntimeServices() {
        if (destination_) aclrtFree(destination_);
        if (event_) aclrtDestroyEvent(event_);
        if (stream_) aclrtDestroyStream(stream_);
        if (selected_) aclrtResetDevice(device_);
        aclFinalize();
    }
    RuntimeServices(const RuntimeServices &) = delete;
    RuntimeServices & operator=(const RuntimeServices &) = delete;

    void prepare(int bytes) {
        check(aclrtSetDevice(device_));
        selected_ = true;
        check(aclrtCreateStream(&stream_));
        check(aclrtCreateEvent(&event_));
        check(aclrtRecordEvent(event_, stream_));
        synchronize();
        if (bytes > 0) {
            // Ordinary host allocation matches the observed unpinned path.
            host_.resize(bytes);
            for (std::size_t i = 0; i < host_.size(); ++i) host_[i] = i % 251;
            check(aclrtMalloc(&destination_, bytes, ACL_MEM_MALLOC_NORMAL_ONLY));
        }
    }

    void synchronize() { check(aclrtSynchronizeStreamWithTimeout(stream_, -1)); }

    double measure(int bytes, int batch) {
        const auto begin = std::chrono::steady_clock::now();
        for (int i = 0; i < batch; ++i) {
            if (bytes == 0) synchronize();
            else check(aclrtMemcpyAsync(destination_, bytes, host_.data(), bytes, ACL_MEMCPY_HOST_TO_DEVICE, stream_));
        }
        const auto elapsed = std::chrono::steady_clock::now() - begin;
        return std::chrono::duration<double, std::nano>(elapsed).count() / batch;
    }

    void verify(int bytes) const {
        if (bytes == 0) return;
        check(aclrtSynchronizeStreamWithTimeout(stream_, -1));
        std::vector<unsigned char> copied(bytes);
        check(aclrtMemcpy(copied.data(), bytes, destination_, bytes, ACL_MEMCPY_DEVICE_TO_HOST));
        if (!std::equal(copied.begin(), copied.end(), host_.begin())) throw std::runtime_error("Host upload verification failed");
    }

private:
    int device_;
    bool selected_ = false;
    aclrtStream stream_ = nullptr;
    aclrtEvent event_ = nullptr;
    std::vector<unsigned char> host_;
    void * destination_ = nullptr;
};

} // namespace

int main(int argc, char ** argv) {
    try {
        if (argc < 4) throw std::runtime_error("usage: markov_runtime_services DEVICE BATCH REPEATS [UPLOAD_BYTES ...]");
        const auto device = argument(argv[1], 0), batch = argument(argv[2], 1), repeats = argument(argv[3], 1);
        std::vector<int> sizes;
        for (int i = 4; i < argc; ++i) sizes.push_back(argument(argv[i], 1));
        if (sizes.empty()) sizes.push_back(0);
        RuntimeServices measurement(device);
        measurement.prepare(*std::max_element(sizes.begin(), sizes.end()));
        std::vector<std::vector<double>> samples;
        for (const auto bytes : sizes) {
            measurement.measure(bytes, 32);
            auto & values = samples.emplace_back();
            for (int i = 0; i < repeats; ++i) values.push_back(measurement.measure(bytes, batch));
            measurement.verify(bytes);
        }

        // Publish only a complete measurement; initialization, warmup and
        // byte verification stay outside every timed interval.
        std::cout << std::setprecision(12)
                  << "{\"clock\":\"steady_clock\",\"stream_state\":\"completed_event_no_pending_work\",\"warmup\":32,\"device\":" << device
                  << ",\"batch\":" << batch << ",\"byte_verification\":true,\"measurements\":[";
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            if (i) std::cout << ',';
            if (sizes[i] == 0)
                std::cout << "{\"operation\":\"runtime.stream_sync\",\"implementation\":\"aclrtSynchronizeStreamWithTimeout\","
                             "\"timing_scope\":\"idle_stream_sync_interval\",\"features\":{\"previous_work_completed\":1,\"timeout_ms\":-1}";
            else
                std::cout << "{\"operation\":\"runtime.copy\",\"implementation\":\"aclrtMemcpyAsync\",\"timing_scope\":\"host_to_device_interval\","
                             "\"features\":{\"host_pinned\":0,\"previous_work_completed\":1,\"bytes\":"
                          << sizes[i] << '}';
            std::cout << ",\"samples_ns\":[";
            for (std::size_t j = 0; j < samples[i].size(); ++j) {
                if (j) std::cout << ',';
                std::cout << samples[i][j];
            }
            std::cout << "]}";
        }
        std::cout << "]}\n";
    }
    catch (const std::exception & error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
