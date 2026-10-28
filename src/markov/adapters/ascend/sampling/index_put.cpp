#include <acl/acl.h>
#include <aclnnop/aclnn_index_put_impl.h>
#include <aclnnop/aclnn_mul.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <time.h>
#include <utility>
#include <vector>

namespace {

void check(int status, const char * operation) {
    if (status) throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(status));
}

std::int64_t number(const char * argument) {
    const std::string text(argument);
    std::int64_t value;
    const auto [end, status] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (status != std::errc{} || end != text.data() + text.size() || value < 0) throw std::invalid_argument("Expected a nonnegative integer");
    return value;
}

std::int64_t clock_ns(clockid_t clock) {
    timespec now;
    check(clock_gettime(clock, &now), "clock_gettime");
    return now.tv_sec * 1'000'000'000LL + now.tv_nsec;
}

struct Sample {
    std::int64_t wall_ns, cpu_ns;
};

class Device {
public:
    explicit Device(int id) {
        check(aclInit(nullptr), "aclInit");
        check(aclrtSetDevice(id), "aclrtSetDevice");
        check(aclrtCreateStream(&stream), "aclrtCreateStream");
    }
    ~Device() {
        aclrtDestroyStream(stream);
        aclFinalize();
    }
    Device(const Device &) = delete;
    Device & operator=(const Device &) = delete;
    aclrtStream stream{};
};

class Buffer {
public:
    explicit Buffer(std::uint64_t size) {
        if (size) check(aclrtMalloc(&data, size, ACL_MEM_MALLOC_HUGE_FIRST), "aclrtMalloc");
    }
    ~Buffer() {
        if (data) aclrtFree(data);
    }
    Buffer(const Buffer &) = delete;
    Buffer & operator=(const Buffer &) = delete;
    void * data = nullptr;
};

class Tensor {
public:
    Tensor(std::vector<std::int64_t> dimensions, aclDataType dtype, std::uint64_t bytes, const void * host = nullptr)
        : dimensions_(std::move(dimensions)),
          strides_(dimensions_.size(), 1),
          buffer_(bytes) {
        for (std::size_t id = dimensions_.size() - 1; id > 0; --id) strides_[id - 1] = strides_[id] * dimensions_[id];
        if (host) check(aclrtMemcpy(buffer_.data, bytes, host, bytes, ACL_MEMCPY_HOST_TO_DEVICE), "aclrtMemcpy input");
        else check(aclrtMemset(buffer_.data, bytes, 0, bytes), "aclrtMemset input");
        handle = aclCreateTensor(dimensions_.data(),
                                 dimensions_.size(),
                                 dtype,
                                 strides_.data(),
                                 0,
                                 ACL_FORMAT_ND,
                                 dimensions_.data(),
                                 dimensions_.size(),
                                 buffer_.data);
        if (!handle) throw std::runtime_error("aclCreateTensor failed");
    }
    ~Tensor() { aclDestroyTensor(handle); }
    Tensor(const Tensor &) = delete;
    Tensor & operator=(const Tensor &) = delete;
    void * data() const { return buffer_.data; }
    aclTensor * handle = nullptr;

private:
    std::vector<std::int64_t> dimensions_, strides_;
    Buffer buffer_;
};

} // namespace

int main(int argc, char ** argv) {
    try {
        if (argc != 6) throw std::invalid_argument("usage: markov_index_put_sample device rows columns repeats output.csv");
        const auto device_id = number(argv[1]), rows = number(argv[2]), columns = number(argv[3]), repeats = number(argv[4]);
        if (device_id > std::numeric_limits<int>::max() || rows < 2 || !columns || !repeats || rows > std::numeric_limits<std::int64_t>::max() / columns / 4)
            throw std::invalid_argument("Invalid device, tensor dimensions or repeat count");
        if (std::filesystem::exists(argv[5])) throw std::invalid_argument("Measurement output already exists");

        Device device(static_cast<int>(device_id));
        const std::int64_t row_index = 1, column_index = columns / 2;
        const std::int32_t value = 17;
        const auto bytes = static_cast<std::uint64_t>(rows * columns * 4);
        Tensor table({ rows, columns }, ACL_INT32, bytes);
        Tensor row({ 1 }, ACL_INT64, 8, &row_index), column({ 1 }, ACL_INT64, 8, &column_index);
        Tensor values({ 1 }, ACL_INT32, 4, &value), initialized({ 1 }, ACL_INT32, 4);
        std::array<const aclTensor *, 2> indices{ row.handle, column.handle };
        std::unique_ptr<aclTensorList, decltype(&aclDestroyTensorList)> list(aclCreateTensorList(indices.data(), indices.size()), aclDestroyTensorList);
        if (!list) throw std::runtime_error("aclCreateTensorList failed");

        // Initialize common CANN submission with a different operation; no
        // IndexPut callback occurs before the first measured invocation.
        std::uint64_t size = 0;
        aclOpExecutor * executor = nullptr;
        check(aclnnMulGetWorkspaceSize(values.handle, values.handle, initialized.handle, &size, &executor), "aclnnMulGetWorkspaceSize");
        {
            Buffer workspace(size);
            check(aclnnMul(workspace.data, size, executor, device.stream), "aclnnMul");
            check(aclrtSynchronizeStream(device.stream), "seed synchronize");
        }

        std::vector<Sample> samples;
        for (std::int64_t repeat = 0; repeat <= repeats; ++repeat) {
            check(aclnnIndexPutImplGetWorkspaceSize(table.handle, list.get(), values.handle, false, false, &size, &executor),
                  "aclnnIndexPutImplGetWorkspaceSize");
            Buffer workspace(size);
            const auto wall_begin = clock_ns(CLOCK_MONOTONIC_RAW);
            const auto cpu_begin = clock_ns(CLOCK_THREAD_CPUTIME_ID);
            const auto status = aclnnIndexPutImpl(workspace.data, size, executor, device.stream);
            const Sample sample{ clock_ns(CLOCK_MONOTONIC_RAW) - wall_begin, clock_ns(CLOCK_THREAD_CPUTIME_ID) - cpu_begin };
            check(status, "aclnnIndexPutImpl");
            check(aclrtSynchronizeStream(device.stream), "measurement synchronize");
            samples.push_back(sample);
        }
        list.reset();

        std::vector<std::int32_t> result(rows * columns);
        check(aclrtMemcpy(result.data(), bytes, table.data(), bytes, ACL_MEMCPY_DEVICE_TO_HOST), "aclrtMemcpy result");
        for (std::int64_t id = 0; id < rows * columns; ++id)
            if (result[id] != (id == row_index * columns + column_index ? value : 0)) throw std::runtime_error("Incorrect indexed write");

        std::ofstream output(argv[5]);
        if (!output) throw std::runtime_error("Cannot write measurements");
        output << "invocation,wall_ns,cpu_ns\n";
        for (std::size_t id = 0; id < samples.size(); ++id) output << id << ',' << samples[id].wall_ns << ',' << samples[id].cpu_ns << '\n';
        if (!output) throw std::runtime_error("Cannot finish measurements");
        std::cout << samples.size() << " synchronized callbacks; indexed writes checked\n";
    }
    catch (const std::exception & error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
