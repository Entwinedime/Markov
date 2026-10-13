#include "worker_sampling.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

struct ClockPoint {
    std::int64_t wall_ns;
    std::int64_t thread_cpu_ns;
    std::int64_t wait_cpu_ns;
    std::int64_t probe_cpu_ns;

    static std::int64_t cpu_time() {
        timespec cpu;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
        return static_cast<std::int64_t>(cpu.tv_sec) * 1'000'000'000 + cpu.tv_nsec;
    }

    static ClockPoint now() {
        // Capture the boundary before recording work. Its measured probe cost
        // belongs to the following interval, not to the preceding service.
        const auto thread_cpu_ns = cpu_time();
        CpuCounters counters;
        markov_native_read_cpu_counters(&counters);
        timespec wall;
        clock_gettime(CLOCK_MONOTONIC, &wall);
        return { static_cast<std::int64_t>(wall.tv_sec) * 1'000'000'000 + wall.tv_nsec, thread_cpu_ns, counters.wait_cpu_ns, counters.probe_cpu_ns };
    }
};

struct SubmissionPoint {
    const char * api;
    bool entering;
    ClockPoint clock;
};

struct QueueSample {
    std::uint64_t correlation_id;
    std::int32_t task_type;
    std::optional<std::string> operation;
    bool worker;
    std::int64_t tid;
    ClockPoint begin;
    ClockPoint end;
    int result = -1;
    std::vector<SubmissionPoint> submissions;
    QueueSample * parent = nullptr;
};

class WorkerSampling {
public:
    static WorkerSampling & instance() {
        static WorkerSampling sampling;
        return sampling;
    }

    void start(const char * directory) {
        directory_ = directory;
        enabled_.store(true);
    }

    QueueSample * begin(std::uint64_t correlation_id, std::int32_t task_type, bool worker, const char * operation) {
        if (!enabled_.load() || (!worker && !in_scope_)) return nullptr;
        {
            const std::lock_guard lock(mutex_);
            if (worker) {
                // Queue identity carries the sampling scope across threads.
                if (pending_.erase(correlation_id) == 0) return nullptr;
            }
            else { pending_.insert(correlation_id); }
        }

        auto sample = std::make_unique<QueueSample>();
        sample->correlation_id = correlation_id;
        sample->task_type = task_type;
        if (operation != nullptr) sample->operation = operation;
        sample->worker = worker;
        sample->tid = syscall(SYS_gettid);
        sample->parent = active_;
        active_ = sample.get();
        sample->begin = ClockPoint::now();
        markov_native_cpu_counters().probe_cpu_ns += ClockPoint::cpu_time() - sample->begin.thread_cpu_ns;
        return sample.release();
    }

    void end(QueueSample * handle, int result) {
        const auto end = ClockPoint::now();
        std::unique_ptr<QueueSample> sample(handle);
        sample->end = end;
        sample->result = result;
        active_ = sample->parent;
        sample->parent = nullptr;

        // Buffer after closing the measured interval; do not put journal I/O
        // inside the original queue call or serialize worker execution.
        const std::lock_guard lock(mutex_);
        if (!sample->worker && result != 0) pending_.erase(sample->correlation_id);
        records_.push_back(std::move(*sample));
    }

    static bool scope(bool enabled) { return std::exchange(in_scope_, enabled); }

    static void point(const char * api, bool entering) {
        if (active_ == nullptr || !active_->worker) return;
        const auto clock = ClockPoint::now();
        active_->submissions.push_back({ api, entering, clock });
        // Includes counter/wall-clock reads and vector bookkeeping. The next
        // boundary observes this actual elapsed CPU amount exactly once.
        markov_native_cpu_counters().probe_cpu_ns += ClockPoint::cpu_time() - clock.thread_cpu_ns;
    }

    void flush() {
        std::vector<QueueSample> records;
        {
            const std::lock_guard lock(mutex_);
            records.swap(records_);
        }
        if (records.empty()) return;

        std::filesystem::create_directories(directory_);
        std::ofstream output(directory_ / ("pid-" + std::to_string(getpid()) + ".jsonl"), std::ios::app);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        for (const auto & record : records) {
            output << "{\"operation\":";
            if (record.operation) write_string(output, *record.operation);
            else output << "null";
            output << ",\"correlation_id\":" << record.correlation_id << ",\"task_type\":" << record.task_type << ",\"phase\":\""
                   << (record.worker ? "worker" : "enqueue") << "\",\"pid\":" << getpid() << ",\"tid\":" << record.tid
                   << ",\"start_ns\":" << record.begin.wall_ns << ",\"end_ns\":" << record.end.wall_ns << ",\"cpu_start_ns\":" << record.begin.thread_cpu_ns
                   << ",\"cpu_end_ns\":" << record.end.thread_cpu_ns << ",\"wait_cpu_start_ns\":" << record.begin.wait_cpu_ns
                   << ",\"wait_cpu_end_ns\":" << record.end.wait_cpu_ns << ",\"probe_cpu_start_ns\":" << record.begin.probe_cpu_ns
                   << ",\"probe_cpu_end_ns\":" << record.end.probe_cpu_ns << ",\"result\":" << record.result << ",\"runtime_points\":[";
            bool first = true;
            for (const auto & point : record.submissions) {
                if (!first) output << ',';
                first = false;
                // API names are fixed identifiers supplied by our runtime hooks.
                output << "{\"api\":" << std::quoted(point.api) << ",\"entering\":" << (point.entering ? "true" : "false")
                       << ",\"wall_ns\":" << point.clock.wall_ns << ",\"thread_cpu_ns\":" << point.clock.thread_cpu_ns
                       << ",\"wait_cpu_ns\":" << point.clock.wait_cpu_ns << ",\"probe_cpu_ns\":" << point.clock.probe_cpu_ns << '}';
            }
            output << "]}\n";
        }
    }

private:
    static void write_string(std::ostream & output, const std::string & value) {
        constexpr char hex[] = "0123456789abcdef";
        output << '"';
        for (const unsigned char character : value) {
            if (character < 0x20) output << "\\u00" << hex[character >> 4] << hex[character & 0xf];
            else {
                if (character == '"' || character == '\\') output << '\\';
                output << static_cast<char>(character);
            }
        }
        output << '"';
    }

    inline static thread_local QueueSample * active_ = nullptr;
    inline static thread_local bool in_scope_ = false;
    std::atomic_bool enabled_{ false };
    std::filesystem::path directory_;
    std::mutex mutex_;
    std::vector<QueueSample> records_;
    std::set<std::uint64_t> pending_;
};

} // namespace

WorkerSubmission::WorkerSubmission(const char * api) : api_(api) { WorkerSampling::point(api_, true); }
WorkerSubmission::~WorkerSubmission() { WorkerSampling::point(api_, false); }

extern "C" void markov_native_begin_worker_sampling(const char * directory) { WorkerSampling::instance().start(directory); }

extern "C" int markov_native_worker_scope(int enabled) { return WorkerSampling::scope(enabled != 0); }

extern "C" void * markov_native_queue_begin(std::uint64_t correlation_id, std::int32_t task_type, int worker, const char * operation) {
    return WorkerSampling::instance().begin(correlation_id, task_type, worker != 0, operation);
}

extern "C" void markov_native_queue_end(void * handle, int result) { WorkerSampling::instance().end(static_cast<QueueSample *>(handle), result); }

extern "C" int markov_native_flush_worker_sampling() {
    try {
        WorkerSampling::instance().flush();
        return 0;
    }
    catch (const std::exception & error) {
        std::fprintf(stderr, "Markov worker sampling flush failed: %s\n", error.what());
        return 1;
    }
}
