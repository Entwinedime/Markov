#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <linux/perf_event.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "runtime_timing.hpp"

std::atomic_bool markov::ascend::host_activity{ false };

namespace {

std::uint64_t raw_ns() {
    timespec time{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &time);
    return static_cast<std::uint64_t>(time.tv_sec) * 1'000'000'000 + static_cast<std::uint64_t>(time.tv_nsec);
}

[[noreturn]] void fail(const char * operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

struct WorkerThread {
    pid_t local;
    std::uint32_t kernel;
};

struct TimedRecord {
    perf_event_header header;
    std::uint32_t pid;
    std::uint32_t tid;
    std::uint64_t time;
};

enum class TraceKind { SwitchOut, FutexEnter, FutexExit };

struct TraceLayout {
    std::uint32_t id;
    std::uint32_t first;
    std::uint32_t second;
};

struct WakeLayout {
    std::uint32_t id;
    std::uint32_t recipient;
    std::uint32_t producer;
    std::uint32_t flags;
};

struct TraceSample {
    TraceKind kind;
    std::uint64_t time;
    std::uint64_t first;
    std::uint64_t second;
};

struct TargetWake {
    std::uint64_t time;
    std::uint32_t producer;
    unsigned flags;
};

using WakeRecords = std::map<std::uint32_t, std::vector<TargetWake>>;
constexpr std::array<std::string_view, 3> trace_names{ "switch_out", "futex_enter", "futex_exit" };

perf_event_attr event_options() {
    perf_event_attr options{};
    options.size = sizeof(options);
    options.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_TIME;
    options.disabled = 1;
    options.use_clockid = 1;
    options.clockid = CLOCK_MONOTONIC_RAW;
    return options;
}

perf_event_attr switch_options() {
    auto options = event_options();
    options.type = PERF_TYPE_SOFTWARE;
    options.config = PERF_COUNT_SW_DUMMY;
    options.sample_id_all = 1;
    options.context_switch = 1;
    return options;
}

perf_event_attr trace_options(std::uint32_t id) {
    auto options = event_options();
    options.type = PERF_TYPE_TRACEPOINT;
    options.config = id;
    options.sample_type |= PERF_SAMPLE_RAW;
    options.sample_period = 1;
    return options;
}

int open_event(perf_event_attr & options, pid_t thread, int cpu) {
    const auto descriptor = static_cast<int>(syscall(SYS_perf_event_open, &options, thread, cpu, -1, PERF_FLAG_FD_CLOEXEC));
    if (descriptor < 0) fail("open native scheduler observation");
    return descriptor;
}

bool lost_record(const perf_event_header & header) {
    return header.type == PERF_RECORD_LOST || header.type == PERF_RECORD_LOST_SAMPLES;
}

class PerfBuffer {
public:
    PerfBuffer(perf_event_attr options, pid_t thread, int cpu, std::size_t data_pages) {
        const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        options.watermark = 1;
        options.wakeup_watermark = static_cast<std::uint32_t>(page * (data_pages / 4));
        fd_ = open_event(options, thread, cpu);
        mapping_size_ = page * (data_pages + 1);
        const auto mapping = mmap(nullptr, mapping_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (mapping == MAP_FAILED) {
            const auto saved = errno;
            close(fd_);
            errno = saved;
            fail("map native scheduler buffer");
        }
        ring_ = static_cast<perf_event_mmap_page *>(mapping);
    }

    PerfBuffer(const PerfBuffer &) = delete;
    PerfBuffer & operator=(const PerfBuffer &) = delete;

    ~PerfBuffer() {
        munmap(ring_, mapping_size_);
        close(fd_);
    }

    int descriptor() const { return fd_; }

    void enable() {
        if (ioctl(fd_, PERF_EVENT_IOC_ENABLE, 0) != 0) fail("start native scheduler observation");
    }

    void disable() {
        if (ioctl(fd_, PERF_EVENT_IOC_DISABLE, 0) != 0) fail("stop native scheduler observation");
    }

    template <class Reader> void drain(Reader read_record) {
        const auto head = __atomic_load_n(&ring_->data_head, __ATOMIC_ACQUIRE);
        auto position = ring_->data_tail;
        while (position < head) {
            const auto header = read<perf_event_header>(position);
            if (header.size < sizeof(header) || header.size > head - position)
                throw std::runtime_error("native scheduler buffer has an incomplete record");
            read_record(position, header);
            position += header.size;
        }
        __atomic_store_n(&ring_->data_tail, head, __ATOMIC_RELEASE);
    }

    template <class Record> Record read(std::uint64_t position) const {
        Record record{};
        const auto * data = reinterpret_cast<const char *>(ring_) + ring_->data_offset;
        const auto offset = position % ring_->data_size;
        const auto first = std::min<std::uint64_t>(sizeof(record), ring_->data_size - offset);
        std::memcpy(&record, data + offset, first);
        std::memcpy(reinterpret_cast<char *>(&record) + first, data, sizeof(record) - first);
        return record;
    }

private:
    int fd_;
    std::size_t mapping_size_;
    perf_event_mmap_page * ring_;
};

class RawSample {
public:
    RawSample(const PerfBuffer & buffer, std::uint64_t position, const perf_event_header & header)
        : buffer_(buffer), position_(position) {
        constexpr auto prefix = sizeof(TimedRecord) + sizeof(std::uint32_t);
        if (header.size < prefix) throw std::runtime_error("scheduler tracepoint omits its RAW payload");
        size_ = buffer_.read<std::uint32_t>(position + sizeof(TimedRecord));
        if (size_ > header.size - prefix) throw std::runtime_error("scheduler tracepoint RAW payload exceeds its record");
    }

    std::uint64_t time() const { return buffer_.read<TimedRecord>(position_).time; }

    template <class T> T field(std::uint32_t offset) const {
        if (static_cast<std::uint64_t>(offset) + sizeof(T) > size_)
            throw std::runtime_error("scheduler tracepoint layout exceeds its RAW payload");
        return buffer_.read<T>(position_ + sizeof(TimedRecord) + sizeof(std::uint32_t) + offset);
    }

private:
    const PerfBuffer & buffer_;
    std::uint64_t position_;
    std::uint32_t size_;
};

class ThreadActivity {
public:
    explicit ThreadActivity(WorkerThread thread) : thread_(thread), buffer_(switch_options(), thread.local, -1, 64) {}

    ~ThreadActivity() {
        for (const auto descriptor : traces_) close(descriptor);
    }

    void attach(const TraceLayout * layouts) {
        if (layouts == nullptr) return;
        std::copy_n(layouts, layouts_.size(), layouts_.begin());
        traces_.reserve(layouts_.size());
        for (const auto & layout : layouts_) {
            auto options = trace_options(layout.id);
            const auto descriptor = open_event(options, thread_.local, -1);
            traces_.push_back(descriptor);

            // Both tracepoint and switch records use the same thread buffer and RAW clock.
            if (ioctl(descriptor, PERF_EVENT_IOC_SET_OUTPUT, buffer_.descriptor()) != 0) fail("share worker thread switch buffer");
        }
    }

    void start() {
        buffer_.enable();
        for (const auto descriptor : traces_) {
            if (ioctl(descriptor, PERF_EVENT_IOC_ENABLE, 0) != 0) fail("start worker scheduler tracepoint observation");
        }
        begin_ = raw_ns();
    }

    int descriptor() const { return buffer_.descriptor(); }
    std::uint32_t kernel_thread() const { return thread_.kernel; }

    void drain() {
        buffer_.drain([this](auto position, const auto & header) {
            if (lost_record(header)) lost_ = true;
            if (header.type == PERF_RECORD_SWITCH) {
                if (header.size != sizeof(TimedRecord)) throw std::runtime_error("unexpected worker switch record layout");
                switches_.push_back(buffer_.read<TimedRecord>(position));
            }
            if (header.type == PERF_RECORD_SAMPLE) record_trace(RawSample(buffer_, position, header));
        });
    }

    void stop(std::ofstream & output, std::uint64_t end, const WakeRecords & wakes) {
        for (const auto descriptor : traces_) {
            if (ioctl(descriptor, PERF_EVENT_IOC_DISABLE, 0) != 0) fail("stop worker scheduler tracepoint observation");
        }
        buffer_.disable();
        drain();
        output << "{\"thread\":" << thread_.local << ",\"window_ns\":[" << begin_ << ',' << end
               << "],\"lost\":" << (lost_ ? "true" : "false") << ",\"switches\":[";
        bool first = true;
        for (const auto & event : switches_) {
            if (!std::exchange(first, false)) output << ',';
            const bool running = (event.header.misc & PERF_RECORD_MISC_SWITCH_OUT) == 0;
            output << '[' << event.time << ',' << (running ? "true" : "false") << ']';
        }
        output << "],\"preemptions\":[";
        first = true;
        for (std::size_t index = 1; index < switches_.size(); ++index) {
            const auto & out = switches_[index - 1];
            const auto & in = switches_[index];
            if (!(out.header.misc & PERF_RECORD_MISC_SWITCH_OUT_PREEMPT) || (in.header.misc & PERF_RECORD_MISC_SWITCH_OUT)) continue;
            if (!std::exchange(first, false)) output << ',';
            output << '[' << out.time << ',' << in.time << ']';
        }
        output << ']';
        if (!traces_.empty()) publish_details(output, end, wakes.at(thread_.kernel));
        output << "}\n";
    }

private:
    void publish_details(std::ofstream & output, std::uint64_t end, const std::vector<TargetWake> & wakes) const {
        output << ",\"kernel_thread\":" << thread_.kernel << ",\"kernel_events\":[";
        bool first = true;
        for (const auto & item : kernel_) {
            if (item.time < begin_ || end < item.time) continue;
            if (!std::exchange(first, false)) output << ',';
            output << "[\"" << trace_names[static_cast<std::size_t>(item.kind)] << "\"," << item.time << ',';
            if (item.kind == TraceKind::FutexExit) output << static_cast<std::int64_t>(item.first);
            else output << item.first << ',' << item.second;
            output << ']';
        }
        output << "],\"target_wakes\":[";
        first = true;
        for (const auto & wake : wakes) {
            if (wake.time < begin_ || end < wake.time) continue;
            if (!std::exchange(first, false)) output << ',';
            output << '[' << wake.time << ',' << wake.producer << ',' << wake.flags << ']';
        }
        output << ']';
    }

    void record_trace(const RawSample & sample) {
        const auto layout = std::ranges::find(layouts_, sample.field<std::uint16_t>(0), &TraceLayout::id);
        if (layout == layouts_.end()) throw std::runtime_error("unexpected worker tracepoint identity");
        const auto kind = static_cast<TraceKind>(layout - layouts_.begin());
        const auto first = kind == TraceKind::SwitchOut ? sample.field<std::uint32_t>(layout->first)
                                                        : sample.field<std::uint64_t>(layout->first);
        const auto second = kind == TraceKind::FutexExit ? 0 : sample.field<std::uint64_t>(layout->second);
        kernel_.push_back({kind, sample.time(), first, second});
    }

    WorkerThread thread_;
    PerfBuffer buffer_;
    std::uint64_t begin_{};
    bool lost_ = false;
    std::vector<TimedRecord> switches_;
    std::array<TraceLayout, trace_names.size()> layouts_{};
    std::vector<int> traces_;
    std::vector<TraceSample> kernel_;
};

class WakeActivity {
public:
    WakeActivity(WakeLayout layout, int cpu, const std::string & filter)
        : layout_(layout), buffer_(trace_options(layout.id), -1, cpu, 16) {
        if (ioctl(buffer_.descriptor(), PERF_EVENT_IOC_SET_FILTER, filter.c_str()) != 0) fail("filter observed wake recipients");
    }

    int descriptor() const { return buffer_.descriptor(); }
    void start() { buffer_.enable(); }

    void drain(WakeRecords & records) {
        buffer_.drain([this, &records](auto position, const auto & header) {
            if (lost_record(header)) throw std::runtime_error("lost targeted scheduler wake records");
            if (header.type != PERF_RECORD_SAMPLE) return;
            const RawSample sample(buffer_, position, header);
            const auto recipient = sample.field<std::uint32_t>(layout_.recipient);
            records.at(recipient).push_back({sample.time(), sample.field<std::uint32_t>(layout_.producer),
                                            sample.field<std::uint8_t>(layout_.flags)});
        });
    }

    void stop(WakeRecords & records) {
        buffer_.disable();
        drain(records);
    }

private:
    WakeLayout layout_;
    PerfBuffer buffer_;
};

class Activity {
public:
    Activity(const WorkerThread * threads, std::size_t count, const TraceLayout * layouts,
             const WakeLayout * wake, const int * cpus, std::size_t cpu_count) {
        for (std::size_t index = 0; index < count; ++index) {
            try {
                auto thread = std::make_unique<ThreadActivity>(threads[index]);
                thread->attach(layouts);
                threads_.push_back(std::move(thread));
            } catch (const std::system_error & error) {
                if (error.code().value() != ESRCH) throw;
            }
        }
        if (wake != nullptr) attach_wakes(*wake, cpus, cpu_count);
        for (const auto & buffer : wakes_) buffer->start();
        for (const auto & thread : threads_) thread->start();
        reader_ = std::jthread([this](std::stop_token stop) { read(stop); });
    }

    void stop(const char * path) {
        const auto end = raw_ns();
        reader_.request_stop();
        reader_.join();
        if (failure_) std::rethrow_exception(failure_);
        for (const auto & wake : wakes_) wake->stop(records_);
        for (auto & [thread, records] : records_) std::ranges::sort(records, {}, &TargetWake::time);

        std::ofstream output(path);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        for (const auto & thread : threads_) thread->stop(output, end, records_);
        output.close();
    }

private:
    void attach_wakes(WakeLayout wake, const int * cpus, std::size_t count) {
        std::string filter;
        for (const auto & thread : threads_) {
            const auto recipient = thread->kernel_thread();
            records_.try_emplace(recipient);
            if (!filter.empty()) filter += " || ";
            filter += "pid == " + std::to_string(recipient);
        }
        if (filter.empty() || count == 0) throw std::runtime_error("targeted wake observation has no worker threads or online CPUs");

        // Wake tracepoints execute on the producer CPU, which can lie outside a worker's affinity.
        for (std::size_t index = 0; index < count; ++index) wakes_.push_back(std::make_unique<WakeActivity>(wake, cpus[index], filter));
    }

    void read(std::stop_token stop) {
        try {
            std::vector<pollfd> descriptors;
            for (const auto & thread : threads_) descriptors.push_back({ thread->descriptor(), POLLIN, 0 });
            for (const auto & wake : wakes_) descriptors.push_back({ wake->descriptor(), POLLIN, 0 });
            while (!stop.stop_requested()) {
                const auto result = poll(descriptors.data(), descriptors.size(), 10);
                if (result < 0 && errno != EINTR) fail("poll worker activity");
                for (std::size_t index = 0; index < descriptors.size(); ++index) {
                    if (descriptors[index].revents & POLLIN) {
                        if (index < threads_.size()) threads_[index]->drain();
                        else wakes_[index - threads_.size()]->drain(records_);
                    }
                    if (descriptors[index].revents & POLLHUP) descriptors[index].fd = -1;
                }
            }
        } catch (...) {
            failure_ = std::current_exception();
        }
    }

    std::vector<std::unique_ptr<ThreadActivity>> threads_;
    std::vector<std::unique_ptr<WakeActivity>> wakes_;
    WakeRecords records_;
    std::exception_ptr failure_;
    std::jthread reader_;
};

} // namespace

extern "C" void * markov_thread_activity_start(const WorkerThread * threads, std::size_t count, const TraceLayout * layouts,
                                                const WakeLayout * wake, const int * cpus, std::size_t cpu_count) {
    try {
        auto activity = std::make_unique<Activity>(threads, count, layouts, wake, cpus, cpu_count);
        markov::ascend::host_activity.store(true, std::memory_order_relaxed);
        return activity.release();
    } catch (const std::exception & error) {
        std::fprintf(stderr, "markov: %s\n", error.what());
        return nullptr;
    }
}

extern "C" int markov_thread_activity_stop(void * handle, const char * path) {
    const std::unique_ptr<Activity> activity(static_cast<Activity *>(handle));
    markov::ascend::host_activity.store(false, std::memory_order_relaxed);
    try {
        activity->stop(path);
        return 0;
    } catch (const std::exception & error) {
        std::fprintf(stderr, "markov: %s\n", error.what());
        return -1;
    }
}
