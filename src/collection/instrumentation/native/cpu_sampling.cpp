#include "cpu_sampling.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace markov::collection {
namespace {

constexpr std::size_t clock_calibration_pairs = 32;

std::int64_t clock_ns(clockid_t clock) {
    timespec value;
    clock_gettime(clock, &value);
    return static_cast<std::int64_t>(value.tv_sec) * 1'000'000'000 + value.tv_nsec;
}

struct ThreadClock {
    std::uint64_t scope = 0;
    std::uint32_t observers = 0;
    std::int64_t observer_begin = 0;
    std::int64_t excluded = 0;
    std::uint64_t observer_cycles = 0;
    std::int64_t read_cpu_ns = std::numeric_limits<std::int64_t>::max();

    ThreadClock() {
        // Consecutive reads include the two outer syscall halves that escape
        // pause/resume exclusion. Calibrate before this thread's first scope.
        for (std::size_t index = 0; index < clock_calibration_pairs; ++index) {
            const auto begin = clock_ns(CLOCK_THREAD_CPUTIME_ID);
            const auto end = clock_ns(CLOCK_THREAD_CPUTIME_ID);
            read_cpu_ns = std::min(read_cpu_ns, end - begin);
        }
    }

    void pause() {
        if (observers++ == 0) observer_begin = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    }

    void resume() {
        if (--observers == 0) {
            excluded += clock_ns(CLOCK_THREAD_CPUTIME_ID) - observer_begin;
            ++observer_cycles;
        }
    }
};

thread_local ThreadClock thread_clock;

class Observer {
public:
    Observer() { thread_clock.pause(); }
    ~Observer() { thread_clock.resume(); }
};

struct Timestamp {
    std::int64_t wall;
    std::int64_t cpu;
    std::int64_t excluded;
    std::uint64_t observer_cycles;

    static Timestamp read() {
        // Called inside Observer: its entry already read the thread clock.
        // A second CPU-clock syscall would cancel out in the subtraction.
        return { clock_ns(CLOCK_MONOTONIC), thread_clock.observer_begin - thread_clock.excluded, thread_clock.excluded, thread_clock.observer_cycles };
    }
};

struct ActivityRecord {
    std::uint64_t scope;
    std::uint64_t tid;
    CpuActivityKind kind;
    std::string name;
    std::uint64_t correlation;
    Timestamp begin;
    Timestamp end;
};

class CpuSampling {
public:
    static CpuSampling & instance() {
        static CpuSampling sampling;
        return sampling;
    }

    void start(const char * directory) {
        directory_ = directory;
        enabled_.store(true);
    }

    bool enabled() const { return enabled_.load(); }

    void append(ActivityRecord record) {
        const std::lock_guard lock(mutex_);
        read_costs_.try_emplace(record.tid, thread_clock.read_cpu_ns);
        records_.push_back(std::move(record));
    }

    void flush() {
        std::vector<ActivityRecord> records;
        std::map<std::uint64_t, std::int64_t> read_costs;
        {
            const std::lock_guard lock(mutex_);
            records.swap(records_);
            read_costs = read_costs_;
        }
        if (records.empty()) return;

        std::filesystem::create_directories(directory_);
        std::ofstream output(directory_ / ("pid-" + std::to_string(getpid()) + ".jsonl"), std::ios::app);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        for (const auto & record : records) {
            output << "{\"pid\":" << getpid() << ",\"tid\":" << record.tid << ",\"scope_id\":" << record.scope
                   << ",\"kind\":" << std::quoted(kind_name(record.kind)) << ",\"name\":" << std::quoted(record.name)
                   << ",\"correlation_id\":" << record.correlation << ",\"boundaries\":[";
            write_timestamp(output, record.begin);
            output << ',';
            write_timestamp(output, record.end);
            output << "]}\n";
        }

        // Calibration is shared by a lane, rather than copied into each event.
        std::ofstream clocks(directory_ / ("pid-" + std::to_string(getpid()) + ".clock.json"));
        clocks.exceptions(std::ios::failbit | std::ios::badbit);
        clocks << "{\"pid\":" << getpid() << ",\"clock\":\"CLOCK_THREAD_CPUTIME_ID\",\"sample_pairs\":" << clock_calibration_pairs << ",\"threads\":[";
        bool first = true;
        for (const auto & [tid, duration] : read_costs) {
            if (!first) clocks << ',';
            first = false;
            clocks << "{\"tid\":" << tid << ",\"read_cpu_ns\":" << duration << '}';
        }
        clocks << "]}\n";
    }

private:
    static const char * kind_name(CpuActivityKind kind) {
        constexpr std::array names{ "scope", "operator", "enqueue", "worker", "submission", "query", "wait", "transfer" };
        return names[static_cast<std::size_t>(kind)];
    }

    static void write_timestamp(std::ostream & output, const Timestamp & value) {
        output << "{\"wall_ns\":" << value.wall << ",\"thread_cpu_ns\":" << value.cpu << ",\"observer_cpu_ns\":" << value.excluded
               << ",\"observer_cycles\":" << value.observer_cycles << '}';
    }

    std::atomic_bool enabled_{ false };
    std::filesystem::path directory_;
    std::mutex mutex_;
    std::vector<ActivityRecord> records_;
    std::map<std::uint64_t, std::int64_t> read_costs_;
};

} // namespace

struct CpuActivity::Record {
    ActivityRecord value;
};

bool cpu_sampling_active() { return CpuSampling::instance().enabled() && thread_clock.scope != 0 && thread_clock.observers == 0; }
bool cpu_sampling_enabled() { return CpuSampling::instance().enabled(); }

CpuObserver::CpuObserver() : active_(cpu_sampling_enabled() && thread_clock.scope != 0) {
    if (active_) thread_clock.pause();
}

CpuObserver::~CpuObserver() {
    if (active_) thread_clock.resume();
}

CpuActivity::CpuActivity(CpuActivityKind kind, const char * name, std::uint64_t correlation) {
    if (!cpu_sampling_active()) return;

    const Observer observer;
    const auto begin = Timestamp::read();
    record_ = std::make_unique<Record>(Record{
        { thread_clock.scope, static_cast<std::uint64_t>(syscall(SYS_gettid)), kind, name, correlation, begin, {} }
    });
}

CpuActivity::~CpuActivity() {
    if (!record_) return;

    const Observer observer;
    record_->value.end = Timestamp::read();
    CpuSampling::instance().append(std::move(record_->value));
    // Deallocation is observation work too, including for nested activities.
    record_.reset();
}

CpuScope::CpuScope(std::uint64_t identity, CpuActivityKind kind, const char * name, std::uint64_t correlation)
    : previous_(std::exchange(thread_clock.scope, identity)),
      activity_(kind, name, correlation) {}

CpuScope::~CpuScope() { thread_clock.scope = previous_; }

} // namespace markov::collection

extern "C" void markov_cpu_begin_sampling(const char * directory) { markov::collection::CpuSampling::instance().start(directory); }

extern "C" void * markov_cpu_scope_begin(std::uint64_t identity, const char * name) {
    return new markov::collection::CpuScope(identity, markov::collection::CpuActivityKind::Scope, name);
}

extern "C" void markov_cpu_scope_end(void * handle) { delete static_cast<markov::collection::CpuScope *>(handle); }

extern "C" void markov_cpu_observer_begin() { markov::collection::thread_clock.pause(); }
extern "C" void markov_cpu_observer_end() { markov::collection::thread_clock.resume(); }

extern "C" int markov_cpu_flush() {
    try {
        markov::collection::CpuSampling::instance().flush();
        return 0;
    }
    catch (const std::exception & error) {
        std::fprintf(stderr, "Markov CPU sampling flush failed: %s\n", error.what());
        return 1;
    }
}
