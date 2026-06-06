#pragma once

#include <fstream>
#include <mutex>
#include <stdint.h>
#include <string>
#include <sys/types.h>

namespace HookFrameWork {

class TraceLogger {
public:
    static TraceLogger & Get();

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
