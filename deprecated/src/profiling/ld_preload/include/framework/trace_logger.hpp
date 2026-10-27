#pragma once

#include <fstream>
#include <mutex>
#include <stdint.h>
#include <string>
#include <sys/types.h>

namespace HookFrameWork {

/**
 * @brief 进程内 Chrome trace writer。
 *
 * TraceLogger 只负责把 wrapper 已经采集到的事实写入 JSON，不做建模推断。
 */
class TraceLogger {
public:
    static TraceLogger & Get();

    /** @brief 写入一条 Chrome trace duration event。 */
    void LogEvent(const std::string & name, uint64_t start_us, uint64_t dur_us, pid_t tid, const std::string & args_str);

private:
    TraceLogger();
    ~TraceLogger();

    std::ofstream file_;
    std::mutex mtx_;
    bool first_event_{ true };
    uint32_t pid_{ 0 };
    std::string trace_output_path_;
};

} // namespace HookFrameWork
