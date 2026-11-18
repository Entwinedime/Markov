#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

#include <linux/perf_event.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

std::uint64_t raw_ns() {
    timespec time{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &time);
    return static_cast<std::uint64_t>(time.tv_sec) * 1'000'000'000 + static_cast<std::uint64_t>(time.tv_nsec);
}

[[noreturn]] void fail(const char * operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

struct Switch {
    perf_event_header header;
    std::uint32_t pid;
    std::uint32_t tid;
    std::uint64_t time;
};

class ThreadActivity {
public:
    explicit ThreadActivity(pid_t thread) : thread_(thread) {
        const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        perf_event_attr options{};
        options.type = PERF_TYPE_SOFTWARE;
        options.size = sizeof(options);
        options.config = PERF_COUNT_SW_DUMMY;
        options.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_TIME;
        options.disabled = 1;
        options.sample_id_all = 1;
        options.context_switch = 1;
        options.use_clockid = 1;
        options.clockid = CLOCK_MONOTONIC_RAW;
        options.watermark = 1;
        options.wakeup_watermark = static_cast<std::uint32_t>(page * 16);
        fd_ = static_cast<int>(syscall(SYS_perf_event_open, &options, thread, -1, -1, PERF_FLAG_FD_CLOEXEC));
        if (fd_ < 0) fail("observe worker thread switches");

        // Wake the writer before the ring fills, including during the first prefill.
        mapping_size_ = page * 65;
        const auto mapping = mmap(nullptr, mapping_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (mapping == MAP_FAILED) {
            const auto saved = errno;
            close(fd_);
            errno = saved;
            fail("map worker thread switch buffer");
        }
        ring_ = static_cast<perf_event_mmap_page *>(mapping);
    }

    ThreadActivity(const ThreadActivity &) = delete;
    ThreadActivity & operator=(const ThreadActivity &) = delete;

    ~ThreadActivity() {
        munmap(ring_, mapping_size_);
        close(fd_);
    }

    void start() {
        if (ioctl(fd_, PERF_EVENT_IOC_ENABLE, 0) != 0) fail("start worker thread observation");
        begin_ = raw_ns();
    }

    int descriptor() const { return fd_; }

    void drain() {
        const auto head = __atomic_load_n(&ring_->data_head, __ATOMIC_ACQUIRE);
        auto position = ring_->data_tail;
        while (position < head) {
            const auto header = read<perf_event_header>(position);
            if (header.size < sizeof(header) || header.size > head - position)
                throw std::runtime_error("worker thread switch buffer has an incomplete record");
            if (header.type == PERF_RECORD_LOST || header.type == PERF_RECORD_LOST_SAMPLES) lost_ = true;
            if (header.type == PERF_RECORD_SWITCH) {
                if (header.size != sizeof(Switch)) throw std::runtime_error("unexpected worker switch record layout");
                switches_.push_back(read<Switch>(position));
            }
            position += header.size;
        }
        __atomic_store_n(&ring_->data_tail, head, __ATOMIC_RELEASE);
    }

    void stop(std::ofstream & output) {
        const auto end = raw_ns();
        if (ioctl(fd_, PERF_EVENT_IOC_DISABLE, 0) != 0) fail("stop worker thread observation");
        drain();
        output << "{\"thread\":" << thread_ << ",\"window_ns\":[" << begin_ << ',' << end
               << "],\"lost\":" << (lost_ ? "true" : "false") << ",\"switches\":[";
        bool first = true;
        for (const auto & event : switches_) {
            if (!first) output << ',';
            first = false;
            const bool running = (event.header.misc & PERF_RECORD_MISC_SWITCH_OUT) == 0;
            output << '[' << event.time << ',' << (running ? "true" : "false") << ']';
        }
        output << "]}\n";
    }

private:
    template <class Record> Record read(std::uint64_t position) const {
        Record record{};
        const auto * data = reinterpret_cast<const char *>(ring_) + ring_->data_offset;
        const auto offset = position % ring_->data_size;
        const auto first = std::min<std::uint64_t>(sizeof(record), ring_->data_size - offset);
        std::memcpy(&record, data + offset, first);
        std::memcpy(reinterpret_cast<char *>(&record) + first, data, sizeof(record) - first);
        return record;
    }

    pid_t thread_;
    int fd_;
    std::size_t mapping_size_;
    perf_event_mmap_page * ring_;
    std::uint64_t begin_{};
    bool lost_ = false;
    std::vector<Switch> switches_;
};

class Activity {
public:
    Activity(const pid_t * threads, std::size_t count) {
        for (std::size_t index = 0; index < count; ++index) {
            try {
                threads_.push_back(std::make_unique<ThreadActivity>(threads[index]));
            } catch (const std::system_error & error) {
                if (error.code().value() != ESRCH) throw;
            }
        }
        for (const auto & thread : threads_) thread->start();
        reader_ = std::jthread([this](std::stop_token stop) { read(stop); });
    }

    void stop(const char * path) {
        reader_.request_stop();
        reader_.join();
        if (failure_) std::rethrow_exception(failure_);
        std::ofstream output(path);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        for (const auto & thread : threads_) thread->stop(output);
        output.close();
    }

private:
    void read(std::stop_token stop) {
        try {
            std::vector<pollfd> descriptors;
            for (const auto & thread : threads_) descriptors.push_back({ thread->descriptor(), POLLIN, 0 });
            while (!stop.stop_requested()) {
                const auto result = poll(descriptors.data(), descriptors.size(), 10);
                if (result < 0 && errno != EINTR) fail("poll worker activity");
                for (std::size_t index = 0; index < descriptors.size(); ++index) {
                    if (descriptors[index].revents & POLLIN) threads_[index]->drain();
                    if (descriptors[index].revents & POLLHUP) descriptors[index].fd = -1;
                }
            }
        } catch (...) {
            failure_ = std::current_exception();
        }
    }

    std::vector<std::unique_ptr<ThreadActivity>> threads_;
    std::exception_ptr failure_;
    std::jthread reader_;
};

} // namespace

extern "C" void * markov_thread_activity_start(const pid_t * threads, std::size_t count) {
    try {
        return std::make_unique<Activity>(threads, count).release();
    } catch (const std::exception & error) {
        std::fprintf(stderr, "markov: %s\n", error.what());
        return nullptr;
    }
}

extern "C" int markov_thread_activity_stop(void * handle, const char * path) {
    const std::unique_ptr<Activity> activity(static_cast<Activity *>(handle));
    try {
        activity->stop(path);
        return 0;
    } catch (const std::exception & error) {
        std::fprintf(stderr, "markov: %s\n", error.what());
        return -1;
    }
}
