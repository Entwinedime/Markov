#pragma once

#include "framework/common.hpp"
#include "framework/hook_arg_info.hpp"
#include "framework/pmu_recorder.hpp"
#include "framework/trace_logger.hpp"
#include <initializer_list>
#include <optional>
#include <sstream>
#include <stdint.h>
#include <string>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#ifndef HOOK_PROFILE_NAME
#define HOOK_PROFILE_NAME "unknown"
#endif

namespace HookFrameWork {

/**
 * @brief wrapper 调用范围计时器。
 *
 * ScopedTimer 在构造时记录开始时间和 PMU 起始快照，在析构时输出一条 Chrome trace duration event。
 * 输出字段描述采集事实和 hook profile，不推导建模策略。
 */
class ScopedTimer {
public:
    explicit ScopedTimer(const std::string & name, std::initializer_list<HookArgInfo> trace_args) : name_(name), start_us_(GetRealtimeUs()) {
        static const bool sync_thread_cpu = ParseEnvFlag("HOOK_SYNC_THREAD_CPU", false);
        static const bool memcpy2d_timing = ParseEnvFlag("HOOK_MEMCPY2D_TIMING", false);
        if ((sync_thread_cpu && name_.starts_with("AscendCL@aclrtSynchronize"))
            || (memcpy2d_timing && name_ == "AscendCL@aclrtMemcpy2dAsync")) start_thread_cpu_ = ThreadCpuNs();
        /**
         * @brief PMU 是可选增强事实；读取失败不影响基础 runtime_op 事件输出。
         */

        pmu_snapshot_ok_ = PmuRecorder::Get().ReadSnapshot(&start_pmu_);

        std::ostringstream oss;
        oss << "\"Function-Args\": {";
        for (const auto & arg_info : trace_args) { oss << "\"" << EscapeJsonString(arg_info.name) << "\": \"" << arg_info.value_in_string << "\", "; }
        std::string function_args_str = oss.str();
        if (function_args_str[function_args_str.length() - 1] == '{') { function_args_str_ = function_args_str + "}"; }
        else { function_args_str_ = function_args_str.substr(0, function_args_str.length() - 2) + "}"; }
    }

    ~ScopedTimer() {
        const auto end_thread_cpu = start_thread_cpu_ ? ThreadCpuNs() : std::nullopt;
        /**
         * @brief 析构时输出事件，确保 wrapper 中真实函数抛出或提前返回时仍能记录已完成范围。
         */
        const pid_t tid{ static_cast<pid_t>(syscall(SYS_gettid)) };
        const uint64_t dur_us{ GetRealtimeUs() - start_us_ };
        const auto cpu_field = end_thread_cpu && *end_thread_cpu >= *start_thread_cpu_
            ? ", \"thread_cpu_ns\": " + std::to_string(*end_thread_cpu - *start_thread_cpu_) : std::string{};
        const std::string args_str{ "\"args\": {"
                                    "\"domain\": \"ld_preload\", "
                                    "\"event_kind\": \"runtime_op\", "
                                    "\"hook_profile\": \""
                                    + std::string(HOOK_PROFILE_NAME)
                                    + "\", "
                                      "\"hook_event_name\": \""
                                    + EscapeJsonString(name_) + "\", " + function_args_str_ + ", " + ProcessPMUSnapshot() + cpu_field + "}" };

        TraceLogger::Get().LogEvent(name_, start_us_, dur_us, tid, args_str);
    }

private:
    static std::optional<uint64_t> ThreadCpuNs() {
        timespec value{};
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) return std::nullopt;
        return static_cast<uint64_t>(value.tv_sec) * 1'000'000'000ULL + value.tv_nsec;
    }
    /** @brief 读取墙钟时间，单位为微秒，对齐 Chrome trace 的 ts/dur 字段。 */
    static uint64_t GetRealtimeUs() {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1'000ULL;
    }

    std::string ProcessPMUSnapshot() {
        /**
         * @brief 输出 PMU delta JSON 字段；快照不可用时输出空 Perf-Events 对象。
         */
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
    std::optional<uint64_t> start_thread_cpu_;
    PmuSnapshot start_pmu_;
    bool pmu_snapshot_ok_{ false };
    std::string function_args_str_;
};

} // namespace HookFrameWork
