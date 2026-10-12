#include "framework/trace_logger.hpp"
#include "framework/common.hpp"
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <time.h>
#include <unistd.h>

namespace {

/** @brief 未配置 HOOK_TRACE_OUTPUT 时使用的默认输出路径。 */
const std::string kDefaultTraceOutputPath{ "cpu_trace.json" };

struct RecorderClock {
    uint64_t wall_ns;
    uint64_t cpu_ns;
};

struct RecorderEmission {
    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t thread_cpu_ns;
};

std::optional<RecorderClock> recorder_clock(bool after_work) {
    timespec wall{}, cpu{};
    // Bracket the CPU-clock samples with wall-clock samples. Sampling wall
    // first at both ends can spuriously report CPU > wall for short writes.
    if (after_work) {
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu) != 0 || clock_gettime(CLOCK_REALTIME, &wall) != 0) return std::nullopt;
    }
    else {
        if (clock_gettime(CLOCK_REALTIME, &wall) != 0 || clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu) != 0) return std::nullopt;
    }
    return RecorderClock{ static_cast<uint64_t>(wall.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(wall.tv_nsec),
                          static_cast<uint64_t>(cpu.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(cpu.tv_nsec) };
}

} // namespace

namespace HookFrameWork {

TraceLogger & TraceLogger::Get() {
    static TraceLogger instance;
    return instance;
}

TraceLogger::TraceLogger() {
    const std::string rank_str{ GetRankString() };
    trace_output_path_ = BuildTraceOutputPath(rank_str, kDefaultTraceOutputPath);

    pid_ = static_cast<uint32_t>(getpid());

    file_.open(trace_output_path_.c_str(), std::ios::out | std::ios::trunc);
    if (file_.is_open()) {
        file_ << "[\n";
        file_ << "{\"name\": \"process_name\", \"ph\": \"M\", \"pid\": " << pid_ << ", \"tid\": 0, \"args\": {\"name\": \"CPU_Hook_rank" << rank_str << "\"}}";
        first_event_ = false;
        file_.flush();
    }
    else { std::cerr << "[hook] Failed to open trace file: " << trace_output_path_ << std::endl; }
}

TraceLogger::~TraceLogger() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (file_.is_open()) {
        file_ << "\n]\n";
        file_.close();
    }
}

void TraceLogger::LogEvent(const std::string & name, uint64_t start_us, uint64_t dur_us, pid_t tid, const std::string & args_str) {
    static const bool measure_emission = ParseEnvFlag("HOOK_EMISSION_TIMING", false);
    thread_local std::optional<RecorderEmission> previous;
    const auto begin = measure_emission ? recorder_clock(false) : std::nullopt;
    {
        std::ostringstream oss;
        std::lock_guard<std::mutex> lock(mtx_);

        if (!file_.is_open()) { return; }
        if (!first_event_) { file_ << ",\n"; }
        first_event_ = false;

        oss << "{\"name\": \"" << EscapeJsonString(name) << "\", \"cat\": \"hook\", \"ph\": \"X\", \"ts\": " << start_us << ", \"dur\": " << dur_us
            << ", \"pid\": " << pid_ << ", \"tid\": " << tid;

        // Carry the completed preceding write on this thread, without an extra
        // event or recursive logging. The final write has no following carrier.
        if (measure_emission && previous)
            oss << ", \"hook_recorder_previous\": {\"start_ns\": " << previous->start_ns << ", \"end_ns\": " << previous->end_ns
                << ", \"thread_cpu_ns\": " << previous->thread_cpu_ns << "}";

        oss << ", " << args_str << "}";
        file_ << oss.str();
        file_.flush();
    }
    if (measure_emission) {
        const auto end = recorder_clock(true);
        previous.reset();
        // Clock failure or adjustment is missing evidence, never zero overhead.
        if (begin && end && end->wall_ns > begin->wall_ns && end->cpu_ns >= begin->cpu_ns && end->cpu_ns - begin->cpu_ns <= end->wall_ns - begin->wall_ns)
            previous = RecorderEmission{ begin->wall_ns, end->wall_ns, end->cpu_ns - begin->cpu_ns };
    }
}

} // namespace HookFrameWork
