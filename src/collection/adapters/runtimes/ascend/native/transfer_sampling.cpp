#include "transfer_sampling.hpp"
#include "cpu_sampling.hpp"

#include <atomic>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

std::int64_t wall_ns() {
    timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return static_cast<std::int64_t>(value.tv_sec) * 1'000'000'000 + value.tv_nsec;
}

struct DriverCopy {
    std::uint64_t bytes;
    std::int32_t direction;
    std::int32_t result;
    std::int64_t begin;
    std::int64_t end;
};

struct QueueOrigin {
    std::uint64_t scope_id;
    bool observer;
};

struct QueueCall {
    std::uint64_t correlation_id;
    std::uint64_t scope_id;
    std::uint64_t tid;
    bool worker;
    bool observer;
    std::int64_t begin;
    std::int64_t end = 0;
    int result = -1;
    std::vector<DriverCopy> copies;
    QueueCall * previous = nullptr;
    std::unique_ptr<markov::collection::CpuObserver> exclusion;
    std::unique_ptr<markov::collection::CpuScope> cpu_scope;
};

class TransferSampling {
public:
    static TransferSampling & instance() {
        static TransferSampling sampling;
        return sampling;
    }

    void start(const char * directory) {
        directory_ = directory;
        enabled_.store(true);
    }

    bool enabled() const { return enabled_.load(); }

    static std::uint64_t scope(std::uint64_t identity) { return std::exchange(scope_, identity); }
    static bool worker_active() { return active_ != nullptr && active_->worker && !active_->observer; }

    QueueCall * begin(std::uint64_t correlation_id, bool worker) {
        if (!enabled()) return nullptr;

        std::unique_ptr<QueueCall> call;
        std::uint64_t scope_id;
        bool observer = markov::collection::cpu_sampling_excluded() || (active_ != nullptr && active_->observer);
        {
            const markov::collection::CpuObserver bookkeeping;
            scope_id = scope_;
            if (scope_id == 0 && active_ != nullptr) scope_id = active_->scope_id;
            {
                const std::lock_guard lock(mutex_);
                if (worker) {
                    const auto found = pending_.find(correlation_id);
                    if (found == pending_.end()) return nullptr;

                    scope_id = found->second.scope_id;
                    observer = found->second.observer;
                    pending_.erase(found);
                }
                else {
                    if (scope_id == 0) return nullptr;
                    pending_.emplace(correlation_id, QueueOrigin{ scope_id, observer });
                }
            }

            call = std::make_unique<QueueCall>(QueueCall{ correlation_id,
                                                          scope_id,
                                                          static_cast<std::uint64_t>(syscall(SYS_gettid)),
                                                          worker,
                                                          observer,
                                                          wall_ns(),
                                                          0,
                                                          -1,
                                                          {},
                                                          active_,
                                                          nullptr,
                                                          nullptr });
        }

        // The original enqueue identity carries exclusion to the worker. Its
        // CPU clocks and driver copies must not become business samples.
        if (observer) call->exclusion = std::make_unique<markov::collection::CpuObserver>(true);
        else if (markov::collection::cpu_sampling_enabled())
            call->cpu_scope = std::make_unique<markov::collection::CpuScope>(scope_id,
                                                                             worker ? markov::collection::CpuActivityKind::Worker
                                                                                    : markov::collection::CpuActivityKind::Enqueue,
                                                                             "torch_npu_queue",
                                                                             correlation_id);
        active_ = call.get();
        return call.release();
    }

    void end(QueueCall * handle, int result) {
        if (handle == nullptr) return;

        const markov::collection::CpuObserver observer;
        std::unique_ptr<QueueCall> call(handle);
        call->end = wall_ns();
        call->cpu_scope.reset();
        call->exclusion.reset();
        call->result = result;
        active_ = call->previous;
        if (call->observer) return;

        const std::lock_guard lock(mutex_);
        records_.push_back(std::move(*call));
    }

    static void append(DriverCopy copy) { active_->copies.push_back(copy); }

    void flush() {
        std::vector<QueueCall> records;
        {
            const std::lock_guard lock(mutex_);
            records.swap(records_);
        }
        if (records.empty()) return;

        std::filesystem::create_directories(directory_);
        std::ofstream output(directory_ / ("pid-" + std::to_string(getpid()) + ".jsonl"), std::ios::app);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        for (const auto & record : records) {
            output << "{\"phase\":\"" << (record.worker ? "worker" : "enqueue") << "\",\"pid\":" << getpid() << ",\"tid\":" << record.tid
                   << ",\"correlation_id\":" << record.correlation_id << ",\"scope_id\":" << record.scope_id << ",\"start_ns\":" << record.begin
                   << ",\"end_ns\":" << record.end << ",\"result\":" << record.result << ",\"driver_copies\":[";
            for (std::size_t index = 0; index < record.copies.size(); ++index) {
                if (index != 0) output << ',';
                const auto & copy = record.copies[index];
                output << "{\"bytes\":" << copy.bytes << ",\"direction\":" << copy.direction << ",\"result\":" << copy.result
                       << ",\"boundaries\":[{\"wall_ns\":" << copy.begin << "},{\"wall_ns\":" << copy.end << "}]}";
            }
            output << "]}\n";
        }
    }

private:
    inline static thread_local std::uint64_t scope_ = 0;
    inline static thread_local QueueCall * active_ = nullptr;
    std::atomic_bool enabled_{ false };
    std::filesystem::path directory_;
    std::mutex mutex_;
    std::unordered_map<std::uint64_t, QueueOrigin> pending_;
    std::vector<QueueCall> records_;
};

} // namespace

bool markov_transfer_sampling_enabled() { return TransferSampling::instance().enabled(); }

TransferDriverCopy::TransferDriverCopy(std::uint64_t bytes, std::int32_t direction) : bytes_(bytes), direction_(direction) {
    if (TransferSampling::worker_active()) begin_ = wall_ns();
}

void TransferDriverCopy::finish(std::int32_t result) {
    if (begin_) TransferSampling::append({ bytes_, direction_, result, *begin_, wall_ns() });
}

extern "C" void markov_native_begin_transfer_sampling(const char * directory) { TransferSampling::instance().start(directory); }

extern "C" std::uint64_t markov_native_transfer_scope(std::uint64_t identity) { return TransferSampling::scope(identity); }

extern "C" void * markov_native_queue_begin(std::uint64_t correlation_id, int worker) {
    return TransferSampling::instance().begin(correlation_id, worker != 0);
}

extern "C" void markov_native_queue_end(void * handle, int result) { TransferSampling::instance().end(static_cast<QueueCall *>(handle), result); }

extern "C" int markov_native_flush_transfer_sampling() {
    try {
        TransferSampling::instance().flush();
        return 0;
    }
    catch (const std::exception & error) {
        std::fprintf(stderr, "Markov transfer sampling flush failed: %s\n", error.what());
        return 1;
    }
}
