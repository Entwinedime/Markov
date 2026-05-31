#pragma once

#include "framework/common.hpp"
#include "framework/hook_arg_info.hpp"
#include "framework/pmu_recorder.hpp"
#include "framework/trace_logger.hpp"
#include <initializer_list>
#include <sstream>
#include <stdint.h>
#include <string>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace HookFrameWork {

class ScopedTimer {
public:
    explicit ScopedTimer(const std::string & name, std::initializer_list<HookArgInfo> trace_args) : name_(name), start_us_(GetRealtimeUs()) {

        pmu_snapshot_ok_ = PmuRecorder::Get().ReadSnapshot(&start_pmu_);

        std::ostringstream oss;
        oss << "\"Function-Args\": {";
        for (const auto & arg_info : trace_args) { oss << "\"" << EscapeJsonString(arg_info.name) << "\": \"" << arg_info.value_in_string << "\", "; }
        std::string function_args_str = oss.str();
        if (function_args_str[function_args_str.length() - 1] == '{') { function_args_str_ = function_args_str + "}"; }
        else { function_args_str_ = function_args_str.substr(0, function_args_str.length() - 2) + "}"; }
    }

    ~ScopedTimer() {
        const pid_t tid{ static_cast<pid_t>(syscall(SYS_gettid)) };
        const uint64_t dur_us{ GetRealtimeUs() - start_us_ };
        const std::string args_str{ "\"args\": {" + function_args_str_ + ", " + ProcessPMUSnapshot() + "}" };

        TraceLogger::Get().LogEvent(name_, start_us_, dur_us, tid, args_str);
    }

private:
    static uint64_t GetRealtimeUs() {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1'000ULL;
    }

    std::string ProcessPMUSnapshot() {
        PmuSnapshot end_pmu;

        std::ostringstream oss;
        oss << "\"Perf-Events\": {";

        const bool can_emit_pmu{ pmu_snapshot_ok_ && PmuRecorder::Get().ReadSnapshot(&end_pmu) && end_pmu.IsCompatibleWith(start_pmu_) };
        if (can_emit_pmu) {
            std::vector<long long> pmu_deltas = end_pmu.DeltaFrom(start_pmu_);
            if (pmu_deltas.size() != end_pmu.event_names.size()) { return "PMU data error"; }
            for (size_t i = 0; i < end_pmu.event_names.size(); ++i) {
                oss << "\"" << EscapeJsonString(end_pmu.event_names[i]) << "\": " << pmu_deltas[i] << ", ";
            }
        }

        std::string result = oss.str();
        if (result[result.length() - 1] == '{') { result += "}"; }
        else { result = result.substr(0, result.length() - 2) + "}"; }
        return result;
    }

    const std::string name_;
    const uint64_t start_us_;
    PmuSnapshot start_pmu_;
    bool pmu_snapshot_ok_{ false };
    std::string function_args_str_;
};

} // namespace HookFrameWork
