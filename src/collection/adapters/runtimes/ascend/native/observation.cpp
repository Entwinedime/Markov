#include "worker_sampling.hpp"

#include <driver/ascend_hal_base.h>
#include <runtime/dev.h>
#include <runtime/event.h>
#include <runtime/kernel.h>
#include <runtime/mem.h>
#include <runtime/rt_ffts_plus.h>
#include <runtime/rt_stars.h>
#include <runtime/rts/rts_kernel.h>
#include <runtime/stream.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

template <typename Function> Function resolve(const char * name) {
    const auto symbol = dlsym(RTLD_NEXT, name);
    if (symbol == nullptr) {
        // This is the external dynamic-link boundary, not an internal invariant.
        std::fprintf(stderr, "Markov native observation cannot resolve %s: %s\n", name, dlerror());
        std::abort();
    }

    return reinterpret_cast<Function>(symbol);
}

std::int64_t clock_ns(clockid_t clock) {
    timespec value;
    clock_gettime(clock, &value);
    return static_cast<std::int64_t>(value.tv_sec) * 1'000'000'000 + value.tv_nsec;
}

struct FftsContext {
    std::uint16_t type;
    std::uint8_t predecessors;
    std::uint16_t thread_id;
    std::uint16_t thread_dim;
    bool auto_thread;
    std::vector<std::uint16_t> successors;
};

class FftsSubmission {
public:
    explicit FftsSubmission(const rtFftsPlusTaskInfo_t & task)
        : address_type_(task.descAddrType),
          context_count_(task.fftsPlusSqe->totalContextNum),
          ready_count_(task.fftsPlusSqe->readyContextNum),
          type_(task.fftsPlusSqe->fftsType) {
        // Never copy a device descriptor to the host: observing dependencies
        // must not introduce a transfer or synchronization into native serving.
        if (address_type_ != RT_FFTS_PLUS_CTX_DESC_ADDR_TYPE_HOST) {
            gap_ = "device_descriptor_not_observed";
            return;
        }
        if (task.descBufLen < context_count_ * sizeof(rtFftsPlusComCtx_t)) {
            gap_ = "descriptor_extent_incomplete";
            return;
        }

        const auto * bytes = static_cast<const unsigned char *>(task.descBuf);
        for (std::size_t index = 0; index < context_count_; ++index) {
            rtFftsPlusComCtx_t context;
            std::memcpy(&context, bytes + index * sizeof(context), sizeof(context));

            // HCCL's unconditional LABEL placeholders share this layout too.
            // Conditional/loop contexts still need their own semantics.
            if (context.contextType != RT_CTX_TYPE_SDMA && context.contextType != RT_CTX_TYPE_NOTIFY_WAIT && context.contextType != RT_CTX_TYPE_NOTIFY_RECORD
                && context.contextType != RT_CTX_TYPE_WRITE_VALUE && context.contextType != RT_CTX_TYPE_LABEL) {
                gap_ = "context_type_not_supported";
                contexts_.clear();
                return;
            }
            if (context.successorNum > RT_CTX_SUCCESSOR_NUM) {
                gap_ = "successor_extent_invalid";
                contexts_.clear();
                return;
            }

            contexts_.push_back({
                context.contextType,
                context.predCntInit,
                context.threadId,
                context.threadDim,
                context.aten != 0,
                { context.successorList, context.successorList + context.successorNum }
            });
        }
    }

    void write(std::ostream & stream) const {
        stream << "{\"address_type\":" << address_type_ << ",\"context_count\":" << context_count_ << ",\"ready_count\":" << ready_count_
               << ",\"ffts_type\":" << type_ << ",\"gap\":";
        if (gap_) stream << '"' << gap_ << '"';
        else stream << "null";

        stream << ",\"contexts\":[";
        for (std::size_t index = 0; index < contexts_.size(); ++index) {
            if (index != 0) stream << ',';
            const auto & context = contexts_[index];
            stream << "{\"id\":" << index << ",\"type\":" << context.type << ",\"predecessors\":" << static_cast<unsigned>(context.predecessors)
                   << ",\"thread_id\":" << context.thread_id << ",\"thread_dim\":" << context.thread_dim
                   << ",\"auto_thread\":" << (context.auto_thread ? "true" : "false") << ",\"successors\":[";
            for (std::size_t successor = 0; successor < context.successors.size(); ++successor) {
                if (successor != 0) stream << ',';
                stream << context.successors[successor];
            }
            stream << "]}";
        }
        stream << "]}";
    }

private:
    std::uint32_t address_type_;
    std::uint16_t context_count_;
    std::uint16_t ready_count_;
    std::uint16_t type_;
    const char * gap_ = nullptr;
    std::vector<FftsContext> contexts_;
};

struct CopyAddresses {
    std::uintptr_t source;
    std::uintptr_t destination;
};

struct DriverCopy {
    CopyAddresses addresses;
    std::uint64_t bytes;
    std::int64_t start_ns;
    std::int64_t end_ns;
    std::int64_t thread_cpu_ns;
    std::int32_t result;
};

// A driver call belongs to the enclosing runtime copy on this thread only.
// RAII restores nesting and never keeps a reference past the API observation.
class DriverCopyScope {
public:
    explicit DriverCopyScope(std::vector<DriverCopy> & records) : previous_(active_) { active_ = &records; }
    ~DriverCopyScope() { active_ = previous_; }
    DriverCopyScope(const DriverCopyScope &) = delete;
    DriverCopyScope & operator=(const DriverCopyScope &) = delete;

    static std::vector<DriverCopy> * active() { return active_; }

private:
    std::vector<DriverCopy> * previous_;
    inline static thread_local std::vector<DriverCopy> * active_ = nullptr;
};

struct WorkspaceTransition {
    int device;
    std::uint64_t requested_bytes;
    std::uint64_t capacity_before;
    std::uintptr_t address_before;
    bool cached;
    bool release;
    std::optional<std::uint64_t> capacity_after;
    std::optional<std::uintptr_t> address_after;
};

struct ApiRecord {
    const char * api;
    std::uint64_t call_id;
    std::uint64_t tid;
    std::uint64_t scope_call_id;
    std::uintptr_t stream_handle;
    std::uintptr_t event_handle;
    std::optional<std::int32_t> stream_id;
    std::optional<std::uint64_t> transfer_bytes;
    std::optional<std::int32_t> transfer_kind;
    std::optional<CopyAddresses> copy_addresses;
    std::optional<std::uint32_t> submitted_task_id;
    std::optional<std::uint32_t> submitted_stream_id;
    std::int64_t wall_time_ns;
    std::int64_t start_ns;
    std::int64_t end_ns;
    std::int64_t thread_cpu_ns;
    rtError_t result;
    std::optional<FftsSubmission> ffts;
    std::vector<DriverCopy> driver_copies;
    std::optional<WorkspaceTransition> workspace;
    std::optional<rtEventStatus_t> event_status;
    bool initial_event;
};

enum class ObservationMode { Off, EventHistory, Journal, CpuSampling };

// Before the serving window, retain only event lifecycle history. Device
// sampling includes its declared warmups; CPU counter mode writes no journal.
// Buffering keeps file I/O out of each API; explicit scope/profile flushes persist
// records before multiprocessing shutdown can bypass destructors.
class NativeJournal {
public:
    static NativeJournal & instance() {
        static NativeJournal journal;
        return journal;
    }

    void begin_window(const char * directory) {
        directory_ = directory;
        window_begin_ns_ = clock_ns(CLOCK_MONOTONIC);
        mode_.store(ObservationMode::Journal);
    }

    bool enabled() const { return mode_.load() == ObservationMode::Journal; }
    bool initial_events() const { return mode_.load() == ObservationMode::EventHistory; }
    bool observes_events() const {
        const auto mode = mode_.load();
        return mode == ObservationMode::EventHistory || mode == ObservationMode::Journal;
    }
    bool cpu_sampling() const { return mode_.load() == ObservationMode::CpuSampling; }

    void begin_cpu_sampling() { mode_.store(ObservationMode::CpuSampling); }

    CpuCounters & cpu_counters() {
        static thread_local CpuCounters counters;
        return counters;
    }

    std::uint64_t next_id() { return sequence_.fetch_add(1); }

    std::uint64_t & current_scope() {
        static thread_local std::uint64_t identity = 0;
        return identity;
    }

    void append(ApiRecord record) {
        const std::lock_guard lock(mutex_);
        records_.push_back(std::move(record));
    }

    void flush(bool close_window) {
        if (close_window) mode_.store(ObservationMode::Off);

        // Release the journal lock before file I/O so API observers can keep appending.
        std::vector<ApiRecord> records;
        {
            const std::lock_guard lock(mutex_);
            records.swap(records_);
        }

        if (records.empty() && !window_begin_ns_) return;

        std::filesystem::create_directories(directory_);
        const auto filename = "pid-" + std::to_string(getpid()) + ".jsonl";
        std::ofstream journal(directory_ / filename, std::ios::app);
        journal.exceptions(std::ios::failbit | std::ios::badbit);
        std::ofstream history;
        history.exceptions(std::ios::failbit | std::ios::badbit);
        if (window_begin_ns_ || std::any_of(records.begin(), records.end(), [](const auto & record) { return record.initial_event; })) {
            std::filesystem::create_directories(directory_ / "initial_events");
            history.open(directory_ / "initial_events" / filename, std::ios::app);
        }
        if (window_begin_ns_) {
            history << "{\"pid\":" << getpid() << ",\"window_begin_ns\":" << *window_begin_ns_ << "}\n";
            window_begin_ns_.reset();
        }
        for (const auto & record : records) {
            auto & stream = record.initial_event ? history : journal;
            // API names are fixed symbols; every other value is numeric/null.
            stream << "{\"api\":\"" << record.api << "\",\"call_id\":" << record.call_id << ",\"pid\":" << getpid() << ",\"tid\":" << record.tid
                   << ",\"scope_call_id\":";
            if (record.scope_call_id != 0) stream << record.scope_call_id;
            else stream << "null";

            stream << ",\"stream_handle\":" << record.stream_handle << ",\"event_handle\":" << record.event_handle << ",\"stream_id\":";
            if (record.stream_id) stream << *record.stream_id;
            else stream << "null";

            stream << ",\"transfer_bytes\":";
            if (record.transfer_bytes) stream << *record.transfer_bytes;
            else stream << "null";
            stream << ",\"transfer_kind\":";
            if (record.transfer_kind) stream << *record.transfer_kind;
            else stream << "null";

            // Addresses identify one observed copy within its process only.
            stream << ",\"copy_addresses\":";
            if (record.copy_addresses) {
                stream << "{\"source\":" << record.copy_addresses->source << ",\"destination\":" << record.copy_addresses->destination << '}';
            }
            else stream << "null";

            stream << ",\"driver_copies\":[";
            for (std::size_t index = 0; index < record.driver_copies.size(); ++index) {
                if (index != 0) stream << ',';
                const auto & copy = record.driver_copies[index];
                stream << "{\"source\":" << copy.addresses.source << ",\"destination\":" << copy.addresses.destination << ",\"bytes\":" << copy.bytes
                       << ",\"start_ns\":" << copy.start_ns << ",\"end_ns\":" << copy.end_ns << ",\"thread_cpu_ns\":" << copy.thread_cpu_ns
                       << ",\"result\":" << copy.result << '}';
            }
            stream << ']';

            stream << ",\"submitted_task_id\":";
            if (record.submitted_task_id) stream << *record.submitted_task_id;
            else stream << "null";
            stream << ",\"submitted_stream_id\":";
            if (record.submitted_stream_id) stream << *record.submitted_stream_id;
            else stream << "null";

            stream << ",\"wall_time_ns\":" << record.wall_time_ns << ",\"start_ns\":" << record.start_ns << ",\"end_ns\":" << record.end_ns
                   << ",\"thread_cpu_ns\":" << record.thread_cpu_ns << ",\"result\":" << record.result;
            if (std::strcmp(record.api, "rtEventQueryStatus") == 0) {
                stream << ",\"event_status\":";
                if (record.event_status) stream << static_cast<std::int32_t>(*record.event_status);
                else stream << "null";
            }
            if (record.ffts) {
                stream << ",\"ffts\":";
                record.ffts->write(stream);
            }
            if (record.workspace) {
                const auto & workspace = *record.workspace;
                stream << ",\"workspace\":{\"device\":" << workspace.device << ",\"requested_bytes\":" << workspace.requested_bytes
                       << ",\"capacity_before_bytes\":" << workspace.capacity_before << ",\"address_before\":" << workspace.address_before << ",\"policy\":\""
                       << (workspace.cached ? "cached" : "uncached") << "\",\"action\":\"" << (workspace.release ? "release" : "allocate")
                       << "\",\"capacity_after_bytes\":";
                if (workspace.capacity_after) stream << *workspace.capacity_after;
                else stream << "null";
                stream << ",\"address_after\":";
                if (workspace.address_after) stream << *workspace.address_after;
                else stream << "null";
                stream << '}';
            }
            stream << "}\n";
        }
    }

private:
    NativeJournal() {
        // Only capture processes request this journal. Retain event lifecycle
        // facts before warmup finishes, without recording the warmup workload.
        if (const auto * directory = std::getenv("MARKOV_NATIVE_OBSERVATIONS")) {
            directory_ = directory;
            mode_.store(ObservationMode::EventHistory);
        }
    }

    std::atomic<ObservationMode> mode_{ ObservationMode::Off };
    std::atomic_uint64_t sequence_{ 1 };

    std::filesystem::path directory_;
    std::optional<std::int64_t> window_begin_ns_;
    std::mutex mutex_;
    std::vector<ApiRecord> records_;
};

// Only the original synchronous runtime call contributes to wait CPU time.
// These counters never create hardware synchronization or a target service cost.
class CpuWaitObservation {
public:
    CpuWaitObservation()
        : wrapper_start_ns_(clock_ns(CLOCK_THREAD_CPUTIME_ID)),
          original_wall_start_ns_(clock_ns(CLOCK_MONOTONIC)),
          original_start_ns_(clock_ns(CLOCK_THREAD_CPUTIME_ID)) {}

    void finish(rtError_t result) {
        const auto original_cpu_end_ns = clock_ns(CLOCK_THREAD_CPUTIME_ID);
        const auto original_wall_end_ns = clock_ns(CLOCK_MONOTONIC);
        const auto original_cpu_ns = original_cpu_end_ns - original_start_ns_;

        auto & counters = NativeJournal::instance().cpu_counters();
        counters.last_sync_start_ns = original_wall_start_ns_;
        counters.last_sync_end_ns = original_wall_end_ns;
        counters.last_sync_cpu_end_ns = original_cpu_end_ns;

        // Probe work before the original call is outside the CPU suffix after
        // completion; work after it remains part of the measured suffix probes.
        counters.last_sync_probe_cpu_ns = counters.probe_cpu_ns + original_start_ns_ - wrapper_start_ns_;
        counters.last_sync_result = result;
        counters.wait_cpu_ns += original_cpu_ns;
        counters.wait_wall_ns += original_wall_end_ns - original_wall_start_ns_;
        counters.probe_cpu_ns += clock_ns(CLOCK_THREAD_CPUTIME_ID) - wrapper_start_ns_ - original_cpu_ns;
    }

private:
    std::int64_t wrapper_start_ns_;
    std::int64_t original_wall_start_ns_;
    std::int64_t original_start_ns_;
};

class ApiObservation {
public:
    ApiObservation(const char * api, rtStream_t stream, rtEvent_t event, bool submits_task = false, std::optional<std::uint64_t> bytes = std::nullopt,
                   std::optional<std::int32_t> kind = std::nullopt, std::optional<CopyAddresses> copy_addresses = std::nullopt)
        : record_{ api,
                   NativeJournal::instance().next_id(),
                   static_cast<std::uint64_t>(syscall(SYS_gettid)),
                   NativeJournal::instance().current_scope(),
                   reinterpret_cast<std::uintptr_t>(stream),
                   reinterpret_cast<std::uintptr_t>(event),
                   std::nullopt,
                   bytes,
                   kind,
                   copy_addresses,
                   std::nullopt,
                   std::nullopt,
                   0,
                   0,
                   0,
                   0,
                   0,
                   std::nullopt,
                   {},
                   std::nullopt,
                   std::nullopt,
                   NativeJournal::instance().initial_events() },
          submits_task_(submits_task) {
        if (stream != nullptr) {
            static const auto get_id = resolve<decltype(&rtGetStreamId)>("rtGetStreamId");
            std::int32_t identity;
            if (get_id(stream, &identity) == RT_ERROR_NONE) record_.stream_id = identity;
        }

        // Stream metadata query is outside the original API's CPU-time sample.
        // Its observation overhead still needs paired collection before costing.
        thread_start_ns_ = clock_ns(CLOCK_THREAD_CPUTIME_ID);
        record_.wall_time_ns = clock_ns(CLOCK_REALTIME);
        record_.start_ns = clock_ns(CLOCK_MONOTONIC);
    }

    std::vector<DriverCopy> & driver_copies() { return record_.driver_copies; }

    void workspace(WorkspaceTransition transition) { record_.workspace = std::move(transition); }

    void finish_workspace(std::uint64_t capacity_after, const void * address_after, int result) {
        if (result == 0) {
            record_.workspace->capacity_after = capacity_after;
            record_.workspace->address_after = reinterpret_cast<std::uintptr_t>(address_after);
        }
        finish(result);
    }

    void finish(rtError_t result, std::optional<FftsSubmission> ffts = std::nullopt, std::optional<rtEventStatus_t> event_status = std::nullopt) {
        // Keep CPU-clock reads outside the wall interval used to split waits.
        // CPU observations still include probe overhead and are not service costs.
        record_.end_ns = clock_ns(CLOCK_MONOTONIC);
        record_.thread_cpu_ns = clock_ns(CLOCK_THREAD_CPUTIME_ID) - thread_start_ns_;
        record_.result = result;
        record_.ffts = std::move(ffts);
        record_.event_status = event_status;

        if (submits_task_ && result == RT_ERROR_NONE) {
            // The SDK reports the calling thread's last submitted task. Query
            // immediately after the original API, before any other submission.
            // This identity query is outside the original API's CPU sample.
            static const auto get_task = resolve<decltype(&rtGetTaskIdAndStreamID)>("rtGetTaskIdAndStreamID");
            std::uint32_t task;
            std::uint32_t stream;
            if (get_task(&task, &stream) == RT_ERROR_NONE) {
                record_.submitted_task_id = task;
                record_.submitted_stream_id = stream;
            }
        }

        NativeJournal::instance().append(std::move(record_));
    }

private:
    ApiRecord record_;
    std::int64_t thread_start_ns_;
    bool submits_task_;
};

} // namespace

// The source probe crosses this boundary with scalars only; Tensor and allocator
// implementation types remain owned by the installed native runtime.
extern "C" void * markov_native_workspace_begin(int device, void * stream, std::uint64_t requested_bytes, std::uint64_t capacity_before,
                                                const void * address_before, bool cached, bool release) {
    if (!NativeJournal::instance().enabled()) return nullptr;

    auto observation = std::make_unique<ApiObservation>(release ? "workspace_release" : "workspace_allocate", stream, nullptr);
    observation->workspace(
        { device, requested_bytes, capacity_before, reinterpret_cast<std::uintptr_t>(address_before), cached, release, std::nullopt, std::nullopt });
    return observation.release();
}

extern "C" void markov_native_workspace_end(void * handle, std::uint64_t capacity_after, const void * address_after, int result) {
    std::unique_ptr<ApiObservation> observation(static_cast<ApiObservation *>(handle));
    observation->finish_workspace(capacity_after, address_after, result);
}

extern "C" void markov_native_begin_window(const char * directory) { NativeJournal::instance().begin_window(directory); }

extern "C" void markov_native_set_scope(std::uint64_t call_id) { NativeJournal::instance().current_scope() = call_id; }

extern "C" void markov_native_begin_cpu_sampling() { NativeJournal::instance().begin_cpu_sampling(); }

CpuCounters & markov_native_cpu_counters() { return NativeJournal::instance().cpu_counters(); }

extern "C" void markov_native_read_cpu_counters(CpuCounters * counters) { *counters = markov_native_cpu_counters(); }

extern "C" int markov_native_flush(int close_window) {
    try {
        NativeJournal::instance().flush(close_window != 0);
        return 0;
    }
    catch (const std::exception & error) {
        std::fprintf(stderr, "Markov native observation flush failed: %s\n", error.what());
        return 1;
    }
}

extern "C" rtError_t rtEventRecord(rtEvent_t event, rtStream_t stream) {
    WorkerSubmission worker_point("rtEventRecord");
    static const auto original = resolve<decltype(&rtEventRecord)>("rtEventRecord");
    if (!NativeJournal::instance().observes_events()) return original(event, stream);

    ApiObservation observation("rtEventRecord", stream, event, NativeJournal::instance().enabled());
    const auto result = original(event, stream);
    observation.finish(result);
    return result;
}

extern "C" rtError_t rtEventQueryStatus(rtEvent_t event, rtEventStatus_t * status) {
    WorkerSubmission worker_point("rtEventQueryStatus");
    static const auto original = resolve<decltype(&rtEventQueryStatus)>("rtEventQueryStatus");
    if (!NativeJournal::instance().observes_events()) return original(event, status);

    // Preserve the native query and returned state; observing completion must
    // neither synchronize the event nor manufacture a device submission.
    ApiObservation observation("rtEventQueryStatus", nullptr, event);
    const auto result = original(event, status);
    observation.finish(result, std::nullopt, result == RT_ERROR_NONE ? std::optional(*status) : std::nullopt);
    return result;
}

extern "C" rtError_t rtStreamWaitEvent(rtStream_t stream, rtEvent_t event) {
    WorkerSubmission worker_point("rtStreamWaitEvent");
    static const auto original = resolve<decltype(&rtStreamWaitEvent)>("rtStreamWaitEvent");
    if (!NativeJournal::instance().enabled()) return original(stream, event);

    ApiObservation observation("rtStreamWaitEvent", stream, event, true);
    const auto result = original(stream, event);
    observation.finish(result);
    return result;
}

extern "C" rtError_t rtStreamSynchronize(rtStream_t stream) {
    WorkerSubmission worker_point("rtStreamSynchronize");
    static const auto original = resolve<decltype(&rtStreamSynchronize)>("rtStreamSynchronize");
    if (NativeJournal::instance().cpu_sampling()) {
        CpuWaitObservation observation;
        const auto result = original(stream);
        observation.finish(result);
        return result;
    }

    if (!NativeJournal::instance().enabled()) return original(stream);

    ApiObservation observation("rtStreamSynchronize", stream, nullptr);
    const auto result = original(stream);
    observation.finish(result);
    return result;
}

extern "C" rtError_t rtStreamSynchronizeWithTimeout(rtStream_t stream, std::int32_t timeout) {
    WorkerSubmission worker_point("rtStreamSynchronizeWithTimeout");
    static const auto original = resolve<decltype(&rtStreamSynchronizeWithTimeout)>("rtStreamSynchronizeWithTimeout");
    if (NativeJournal::instance().cpu_sampling()) {
        CpuWaitObservation observation;
        const auto result = original(stream, timeout);
        observation.finish(result);
        return result;
    }

    if (!NativeJournal::instance().enabled()) return original(stream, timeout);

    ApiObservation observation("rtStreamSynchronizeWithTimeout", stream, nullptr);
    const auto result = original(stream, timeout);
    observation.finish(result);
    return result;
}

extern "C" rtError_t rtEventSynchronize(rtEvent_t event) {
    WorkerSubmission worker_point("rtEventSynchronize");
    static const auto original = resolve<decltype(&rtEventSynchronize)>("rtEventSynchronize");
    if (NativeJournal::instance().cpu_sampling()) {
        CpuWaitObservation observation;
        const auto result = original(event);
        observation.finish(result);
        return result;
    }

    if (!NativeJournal::instance().enabled()) return original(event);

    ApiObservation observation("rtEventSynchronize", nullptr, event);
    const auto result = original(event);
    observation.finish(result);
    return result;
}

extern "C" rtError_t rtDeviceSynchronize() {
    WorkerSubmission worker_point("rtDeviceSynchronize");
    static const auto original = resolve<decltype(&rtDeviceSynchronize)>("rtDeviceSynchronize");
    if (NativeJournal::instance().cpu_sampling()) {
        CpuWaitObservation observation;
        const auto result = original();
        observation.finish(result);
        return result;
    }

    if (!NativeJournal::instance().enabled()) return original();

    ApiObservation observation("rtDeviceSynchronize", nullptr, nullptr);
    const auto result = original();
    observation.finish(result);
    return result;
}

extern "C" rtError_t rtDeviceSynchronizeWithTimeout(std::int32_t timeout) {
    WorkerSubmission worker_point("rtDeviceSynchronizeWithTimeout");
    static const auto original = resolve<decltype(&rtDeviceSynchronizeWithTimeout)>("rtDeviceSynchronizeWithTimeout");
    if (NativeJournal::instance().cpu_sampling()) {
        CpuWaitObservation observation;
        const auto result = original(timeout);
        observation.finish(result);
        return result;
    }

    if (!NativeJournal::instance().enabled()) return original(timeout);

    ApiObservation observation("rtDeviceSynchronizeWithTimeout", nullptr, nullptr);
    const auto result = original(timeout);
    observation.finish(result);
    return result;
}

extern "C" rtError_t rtEventDestroy(rtEvent_t event) {
    WorkerSubmission worker_point("rtEventDestroy");
    static const auto original = resolve<decltype(&rtEventDestroy)>("rtEventDestroy");

    if (!NativeJournal::instance().observes_events()) return original(event);

    ApiObservation observation("rtEventDestroy", nullptr, event);
    const auto result = original(event);
    observation.finish(result);
    return result;
}

extern "C" rtError_t rtMemcpyAsync(void * destination, std::uint64_t maximum_bytes, const void * source, std::uint64_t bytes, rtMemcpyKind_t kind,
                                   rtStream_t stream) {
    WorkerSubmission worker_point("rtMemcpyAsync");
    static const auto original = resolve<decltype(&rtMemcpyAsync)>("rtMemcpyAsync");
    if (!NativeJournal::instance().enabled()) return original(destination, maximum_bytes, source, bytes, kind, stream);

    ApiObservation observation("rtMemcpyAsync",
                               stream,
                               nullptr,
                               true,
                               bytes,
                               static_cast<std::int32_t>(kind),
                               CopyAddresses{ reinterpret_cast<std::uintptr_t>(source), reinterpret_cast<std::uintptr_t>(destination) });
    DriverCopyScope copies(observation.driver_copies());
    const auto result = original(destination, maximum_bytes, source, bytes, kind, stream);
    observation.finish(result);
    return result;
}

extern "C" DVresult drvMemcpy(DVdeviceptr destination, std::size_t maximum_bytes, DVdeviceptr source, std::size_t bytes) {
    static const auto original = resolve<decltype(&drvMemcpy)>("drvMemcpy");
    auto * records = DriverCopyScope::active();
    if (records == nullptr) return original(destination, maximum_bytes, source, bytes);

    const auto cpu_start = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    const auto start = clock_ns(CLOCK_MONOTONIC);
    const auto result = original(destination, maximum_bytes, source, bytes);
    const auto end = clock_ns(CLOCK_MONOTONIC);
    const auto cpu_end = clock_ns(CLOCK_THREAD_CPUTIME_ID);

    // The driver contract is synchronous. Its entry is after the runtime's
    // internal stream wait, not the beginning of that wait or a pure CPU cost.
    records->push_back({
        { source, destination },
        bytes,
        start,
        end,
        cpu_end - cpu_start,
        static_cast<std::int32_t>(result)
    });
    return result;
}

extern "C" rtError_t rtGeneralCtrl(std::uintptr_t * control, std::uint32_t count, std::uint32_t type) {
    WorkerSubmission worker_point("rtGeneralCtrl");
    static const auto original = resolve<decltype(&rtGeneralCtrl)>("rtGeneralCtrl");
    if (!NativeJournal::instance().enabled() || type != RT_GNL_CTRL_TYPE_FFTS_PLUS || count != 2) return original(control, count, type);

    // Installed HCCL submits [descriptor, stream] through this SDK command.
    // Snapshot before runtime consumption; never interpret other commands.
    const auto * task = reinterpret_cast<const rtFftsPlusTaskInfo_t *>(control[0]);
    const auto stream = reinterpret_cast<rtStream_t>(control[1]);
    FftsSubmission submission(*task);
    ApiObservation observation("rtGeneralCtrl", stream, nullptr, true);
    const auto result = original(control, count, type);
    observation.finish(result, std::move(submission));
    return result;
}

// Compute submission boundaries observed in the current Ascend mainline.
// Forward arguments unchanged; do not inspect tensor memory or synchronize.
extern "C" rtError_t rtKernelLaunch(const void * stub, std::uint32_t blocks, void * args, std::uint32_t args_size, rtSmDesc_t * shared_memory,
                                    rtStream_t stream) {
    static const auto original = resolve<decltype(&rtKernelLaunch)>("rtKernelLaunch");
    WorkerSubmission worker_point("rtKernelLaunch");
    return original(stub, blocks, args, args_size, shared_memory, stream);
}

extern "C" rtError_t rtKernelLaunchWithHandle(void * handle, const std::uint64_t tiling_key, std::uint32_t blocks, rtArgsEx_t * args,
                                              rtSmDesc_t * shared_memory, rtStream_t stream, const void * kernel_info) {
    static const auto original = resolve<decltype(&rtKernelLaunchWithHandle)>("rtKernelLaunchWithHandle");
    WorkerSubmission worker_point("rtKernelLaunchWithHandle");
    return original(handle, tiling_key, blocks, args, shared_memory, stream, kernel_info);
}

extern "C" rtError_t rtKernelLaunchWithHandleV2(void * handle, const std::uint64_t tiling_key, std::uint32_t blocks, rtArgsEx_t * args,
                                                rtSmDesc_t * shared_memory, rtStream_t stream, const rtTaskCfgInfo_t * configuration) {
    static const auto original = resolve<decltype(&rtKernelLaunchWithHandleV2)>("rtKernelLaunchWithHandleV2");
    WorkerSubmission worker_point("rtKernelLaunchWithHandleV2");
    return original(handle, tiling_key, blocks, args, shared_memory, stream, configuration);
}

extern "C" rtError_t rtsLaunchKernelWithHostArgs(rtFuncHandle handle, std::uint32_t blocks, rtStream_t stream, rtKernelLaunchCfg_t * configuration, void * args,
                                                 std::uint32_t args_size, rtPlaceHolderInfo_t * placeholders, std::uint32_t placeholder_count) {
    static const auto original = resolve<decltype(&rtsLaunchKernelWithHostArgs)>("rtsLaunchKernelWithHostArgs");
    WorkerSubmission worker_point("rtsLaunchKernelWithHostArgs");
    return original(handle, blocks, stream, configuration, args, args_size, placeholders, placeholder_count);
}
