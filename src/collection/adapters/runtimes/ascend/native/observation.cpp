#include "cpu_sampling.hpp"
#include "native_symbols.hpp"
#include "transfer_sampling.hpp"

#include "markov/adapters/runtimes/ascend/cpu_identity.hpp"

#include <driver/ascend_hal_base.h>
#include <runtime/dev.h>
#include <runtime/event.h>
#include <runtime/mem.h>
#include <runtime/rt_ffts_plus.h>
#include <runtime/rt_stars.h>
#include <runtime/stream.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

using markov::collection::native_symbol;

std::int64_t clock_ns() {
    timespec value;
    // CANN converts this clock using its recorded clockMonotonicRaw origin.
    clock_gettime(CLOCK_MONOTONIC_RAW, &value);
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
    std::int32_t result;
};

// A driver call belongs to the enclosing runtime copy on this thread only.
// RAII restores nesting and never keeps a reference past the runtime call.
class DriverCopyScope {
public:
    explicit DriverCopyScope(std::int32_t kind, std::vector<DriverCopy> * records = nullptr) : previous_(active_), records_(records), kind_(kind) {
        active_ = this;
    }
    ~DriverCopyScope() { active_ = previous_; }
    DriverCopyScope(const DriverCopyScope &) = delete;
    DriverCopyScope & operator=(const DriverCopyScope &) = delete;

    static DriverCopyScope * active() { return active_; }
    std::vector<DriverCopy> * records() const { return records_; }
    std::int32_t kind() const { return kind_; }

private:
    DriverCopyScope * previous_;
    std::vector<DriverCopy> * records_;
    std::int32_t kind_;
    inline static thread_local DriverCopyScope * active_ = nullptr;
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
    std::int64_t start_ns;
    std::int64_t end_ns;
    rtError_t result;
    std::optional<FftsSubmission> ffts;
    std::vector<DriverCopy> driver_copies;
    std::optional<WorkspaceTransition> workspace;
};

// Only explicitly opened observation windows produce journal records.
// Device sampling includes its declared warmups; host sampling writes no journal.
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
        enabled_.store(true);
    }

    bool enabled() const { return enabled_.load(); }
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
        if (close_window) enabled_.store(false);

        // Release the journal lock before file I/O so API observers can keep appending.
        std::vector<ApiRecord> records;
        {
            const std::lock_guard lock(mutex_);
            records.swap(records_);
        }

        if (records.empty()) return;

        std::filesystem::create_directories(directory_);
        const auto filename = "pid-" + std::to_string(getpid()) + ".jsonl";
        std::ofstream stream(directory_ / filename, std::ios::app);
        stream.exceptions(std::ios::failbit | std::ios::badbit);
        for (const auto & record : records) {
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
                       << ",\"start_ns\":" << copy.start_ns << ",\"end_ns\":" << copy.end_ns << ",\"result\":" << copy.result << '}';
            }
            stream << ']';

            stream << ",\"submitted_task_id\":";
            if (record.submitted_task_id) stream << *record.submitted_task_id;
            else stream << "null";
            stream << ",\"submitted_stream_id\":";
            if (record.submitted_stream_id) stream << *record.submitted_stream_id;
            else stream << "null";

            stream << ",\"start_ns\":" << record.start_ns << ",\"end_ns\":" << record.end_ns << ",\"result\":" << record.result;
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
    NativeJournal() = default;

    std::atomic_bool enabled_{ false };
    std::atomic_uint64_t sequence_{ 1 };

    std::filesystem::path directory_;
    std::mutex mutex_;
    std::vector<ApiRecord> records_;
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
                   std::nullopt,
                   {},
                   std::nullopt },
          submits_task_(submits_task) {
        if (stream != nullptr) {
            static const auto get_id = native_symbol<decltype(&rtGetStreamId)>("rtGetStreamId");
            std::int32_t identity;
            if (get_id(stream, &identity) == RT_ERROR_NONE) record_.stream_id = identity;
        }

        // These wall boundaries associate the API with device work and waits.
        // CPU service is measured separately by the thread-clock sampler.
        record_.start_ns = clock_ns();
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

    void finish(rtError_t result, std::optional<FftsSubmission> ffts = std::nullopt) {
        record_.end_ns = clock_ns();
        record_.result = result;
        record_.ffts = std::move(ffts);

        if (submits_task_ && result == RT_ERROR_NONE) {
            // The SDK reports the calling thread's last submitted task. Query
            // immediately after the original API, before any other submission.
            // This identity query is outside the original API's wall interval.
            static const auto get_task = native_symbol<decltype(&rtGetTaskIdAndStreamID)>("rtGetTaskIdAndStreamID");
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
    static const auto original = native_symbol<decltype(&rtEventRecord)>("rtEventRecord");
    const markov::collection::CpuActivity cpu(markov::collection::CpuActivityKind::Submission, "rtEventRecord");
    if (!NativeJournal::instance().enabled()) return original(event, stream);

    ApiObservation observation("rtEventRecord", stream, event, true);
    const auto result = original(event, stream);
    observation.finish(result);
    return result;
}

extern "C" rtError_t rtEventQueryStatus(rtEvent_t event, rtEventStatus_t * status) {
    static const auto original = native_symbol<decltype(&rtEventQueryStatus)>("rtEventQueryStatus");
    const markov::collection::CpuActivity cpu(markov::collection::CpuActivityKind::Query, "rtEventQueryStatus");
    if (!NativeJournal::instance().enabled()) return original(event, status);

    // Observe the native call without synchronizing or manufacturing a submission.
    ApiObservation observation("rtEventQueryStatus", nullptr, event);
    const auto result = original(event, status);
    observation.finish(result);
    return result;
}

extern "C" rtError_t rtStreamWaitEvent(rtStream_t stream, rtEvent_t event) {
    static const auto original = native_symbol<decltype(&rtStreamWaitEvent)>("rtStreamWaitEvent");
    const markov::collection::CpuActivity cpu(markov::collection::CpuActivityKind::Submission, "rtStreamWaitEvent");
    if (!NativeJournal::instance().enabled()) return original(stream, event);

    ApiObservation observation("rtStreamWaitEvent", stream, event, true);
    const auto result = original(stream, event);
    observation.finish(result);
    return result;
}

extern "C" rtError_t rtStreamSynchronize(rtStream_t stream) {
    static const auto original = native_symbol<decltype(&rtStreamSynchronize)>("rtStreamSynchronize");
    const markov::collection::CpuActivity cpu(markov::collection::CpuActivityKind::Wait, "rtStreamSynchronize");

    if (!NativeJournal::instance().enabled()) return original(stream);

    ApiObservation observation("rtStreamSynchronize", stream, nullptr);
    const auto result = original(stream);
    observation.finish(result);
    return result;
}

extern "C" rtError_t rtStreamSynchronizeWithTimeout(rtStream_t stream, std::int32_t timeout) {
    static const auto original = native_symbol<decltype(&rtStreamSynchronizeWithTimeout)>("rtStreamSynchronizeWithTimeout");
    const markov::collection::CpuActivity cpu(markov::collection::CpuActivityKind::Wait, "rtStreamSynchronizeWithTimeout");

    if (!NativeJournal::instance().enabled()) return original(stream, timeout);

    ApiObservation observation("rtStreamSynchronizeWithTimeout", stream, nullptr);
    const auto result = original(stream, timeout);
    observation.finish(result);
    return result;
}

extern "C" rtError_t rtEventSynchronize(rtEvent_t event) {
    static const auto original = native_symbol<decltype(&rtEventSynchronize)>("rtEventSynchronize");
    const markov::collection::CpuActivity cpu(markov::collection::CpuActivityKind::Wait, "rtEventSynchronize");

    if (!NativeJournal::instance().enabled()) return original(event);

    ApiObservation observation("rtEventSynchronize", nullptr, event);
    const auto result = original(event);
    observation.finish(result);
    return result;
}

extern "C" rtError_t rtDeviceSynchronize() {
    static const auto original = native_symbol<decltype(&rtDeviceSynchronize)>("rtDeviceSynchronize");
    const markov::collection::CpuActivity cpu(markov::collection::CpuActivityKind::Wait, "rtDeviceSynchronize");

    if (!NativeJournal::instance().enabled()) return original();

    ApiObservation observation("rtDeviceSynchronize", nullptr, nullptr);
    const auto result = original();
    observation.finish(result);
    return result;
}

extern "C" rtError_t rtDeviceSynchronizeWithTimeout(std::int32_t timeout) {
    static const auto original = native_symbol<decltype(&rtDeviceSynchronizeWithTimeout)>("rtDeviceSynchronizeWithTimeout");
    const markov::collection::CpuActivity cpu(markov::collection::CpuActivityKind::Wait, "rtDeviceSynchronizeWithTimeout");

    if (!NativeJournal::instance().enabled()) return original(timeout);

    ApiObservation observation("rtDeviceSynchronizeWithTimeout", nullptr, nullptr);
    const auto result = original(timeout);
    observation.finish(result);
    return result;
}

extern "C" rtError_t rtEventDestroy(rtEvent_t event) {
    static const auto original = native_symbol<decltype(&rtEventDestroy)>("rtEventDestroy");

    if (!NativeJournal::instance().enabled()) return original(event);

    ApiObservation observation("rtEventDestroy", nullptr, event);
    const auto result = original(event);
    observation.finish(result);
    return result;
}

extern "C" rtError_t rtMemcpyAsync(void * destination, std::uint64_t maximum_bytes, const void * source, std::uint64_t bytes, rtMemcpyKind_t kind,
                                   rtStream_t stream) {
    static const auto original = native_symbol<decltype(&rtMemcpyAsync)>("rtMemcpyAsync");
    const markov::collection::CpuActivity cpu(markov::collection::CpuActivityKind::Submission, "rtMemcpyAsync");
    if (markov_transfer_sampling_enabled()) {
        DriverCopyScope copies(static_cast<std::int32_t>(kind));
        return original(destination, maximum_bytes, source, bytes, kind, stream);
    }
    if (!NativeJournal::instance().enabled()) return original(destination, maximum_bytes, source, bytes, kind, stream);

    ApiObservation observation("rtMemcpyAsync",
                               stream,
                               nullptr,
                               true,
                               bytes,
                               static_cast<std::int32_t>(kind),
                               CopyAddresses{ reinterpret_cast<std::uintptr_t>(source), reinterpret_cast<std::uintptr_t>(destination) });
    DriverCopyScope copies(static_cast<std::int32_t>(kind), &observation.driver_copies());
    const auto result = original(destination, maximum_bytes, source, bytes, kind, stream);
    observation.finish(result);
    return result;
}

extern "C" DVresult drvMemcpy(DVdeviceptr destination, std::size_t maximum_bytes, DVdeviceptr source, std::size_t bytes) {
    static const auto original = native_symbol<decltype(&drvMemcpy)>("drvMemcpy");
    auto * scope = DriverCopyScope::active();
    if (scope == nullptr) return original(destination, maximum_bytes, source, bytes);

    const markov::collection::CpuActivity cpu(markov::collection::CpuActivityKind::Transfer, markov::ascend_cpu::driver_copy);

    auto * records = scope->records();
    if (records == nullptr) {
        TransferDriverCopy observation(bytes, scope->kind());
        const auto result = original(destination, maximum_bytes, source, bytes);
        observation.finish(static_cast<std::int32_t>(result));
        return result;
    }

    const auto start = clock_ns();
    const auto result = original(destination, maximum_bytes, source, bytes);
    const auto end = clock_ns();

    // The driver contract is synchronous. Its entry is after the runtime's
    // internal stream wait, not the beginning of that wait or a pure CPU cost.
    records->push_back({
        { source, destination },
        bytes,
        start,
        end,
        static_cast<std::int32_t>(result)
    });
    return result;
}

extern "C" rtError_t rtGeneralCtrl(std::uintptr_t * control, std::uint32_t count, std::uint32_t type) {
    static const auto original = native_symbol<decltype(&rtGeneralCtrl)>("rtGeneralCtrl");
    const markov::collection::CpuActivity cpu(markov::collection::CpuActivityKind::Submission, "rtGeneralCtrl");
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
