#include "cli/prediction_memory.hpp"

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

#include <sys/resource.h>

namespace markov::trace_graph::cli {
namespace {

uint64_t field_kib(const char * path, const char * field) {
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        if (line.starts_with(field)) return std::stoull(line.substr(std::string(field).size())) * 1'024;
    }
    throw std::runtime_error(std::string("Cannot read memory field ") + path + ":" + field);
}

uint64_t controller_value(const char * v2, const char * v1) {
    std::ifstream input(v2);
    if (!input) input.open(v1);
    std::string value;
    if (!(input >> value) || value == "max") return std::numeric_limits<uint64_t>::max();
    return std::stoull(value);
}

uint64_t available_bytes() {
    const auto host = field_kib("/proc/meminfo", "MemAvailable:");
    const auto limit = controller_value("/sys/fs/cgroup/memory.max", "/sys/fs/cgroup/memory/memory.limit_in_bytes");
    const auto used = controller_value("/sys/fs/cgroup/memory.current", "/sys/fs/cgroup/memory/memory.usage_in_bytes");
    if (limit == std::numeric_limits<uint64_t>::max()) return host;
    return std::min(host, used < limit ? limit - used : uint64_t{ 0 });
}

void address_limit(uint64_t bytes) {
    rlimit existing{};
    if (getrlimit(RLIMIT_AS, &existing) < 0) throw std::system_error(errno, std::generic_category(), "read address limit");
    existing.rlim_cur = std::min(static_cast<rlim_t>(bytes), existing.rlim_cur);
    if (setrlimit(RLIMIT_AS, &existing) < 0) throw std::system_error(errno, std::generic_category(), "set prediction address limit");
}

} // namespace

PredictionMemory::PredictionMemory()
    : limit_(std::min(field_kib("/proc/meminfo", "MemTotal:"), controller_value("/sys/fs/cgroup/memory.max", "/sys/fs/cgroup/memory/memory.limit_in_bytes"))),
      reserve_(std::max(uint64_t{ 512 } * 1'024 * 1'024, limit_ / 10)) {}

void PredictionMemory::limit_source() const {
    if (limit_ <= reserve_ || pressure()) throw std::runtime_error("Insufficient memory reserve for source preparation");
    address_limit(limit_ - reserve_);
}

void PredictionMemory::prepare_workers(uint64_t jobs) {
    const auto available = available_bytes();
    // Leave one allocation share for preparation buffers and concurrent growth.
    worker_ = available > reserve_ ? (available - reserve_) / (jobs + 1) : 0;
    source_virtual_ = field_kib("/proc/self/status", "VmSize:");
}

void PredictionMemory::limit_worker() const { address_limit(std::min(limit_ - reserve_, source_virtual_ + worker_)); }

bool PredictionMemory::can_start() const {
    const auto available = available_bytes();
    return worker_ && available > reserve_ && available - reserve_ >= worker_;
}

bool PredictionMemory::pressure() const { return available_bytes() <= reserve_; }

} // namespace markov::trace_graph::cli
