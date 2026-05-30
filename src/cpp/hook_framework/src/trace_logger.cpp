#include "framework/trace_logger.hpp"
#include "framework/common.hpp"
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>


namespace {

// 未配置 HOOK_TRACE_OUTPUT 时使用的默认输出路径。
const std::string kDefaultTraceOutputPath{ "cpu_trace.json" };

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
    std::ostringstream oss;
    std::lock_guard<std::mutex> lock(mtx_);

    if (!file_.is_open()) { return; }
    if (!first_event_) { file_ << ",\n"; }
    first_event_ = false;

    oss << "{\"name\": \"" << EscapeJsonString(name) << "\", \"cat\": \"hook\", \"ph\": \"X\", \"ts\": " << start_us << ", \"dur\": " << dur_us
        << ", \"pid\": " << pid_ << ", \"tid\": " << tid;

    oss << ", " << args_str << "}";
    file_ << oss.str();
    file_.flush();
}

} // namespace HookFrameWork
