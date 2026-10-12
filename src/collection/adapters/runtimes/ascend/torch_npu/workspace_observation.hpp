#pragma once

#include <cstdint>
#include <dlfcn.h>
#include <memory>
#include <utility>

#include "torch_npu/csrc/profiler/profiler_mgr.h"
#include "torch_npu/csrc/toolkit/profiler/common/utils.h"
#include "torch_npu/csrc/toolkit/profiler/inc/data_reporter.h"

namespace markov::collection {

// Included by the image-local allocator probe. Without the observation library,
// the native allocator retains its normal behavior and has no journal to update.
class WorkspaceObservation {
public:
    WorkspaceObservation(int device, void * stream, std::uint64_t requested_bytes, std::uint64_t capacity_before, const void * address_before, bool cached,
                         bool release) {
        static const auto begin = reinterpret_cast<Begin>(dlsym(RTLD_DEFAULT, "markov_native_workspace_begin"));
        static const auto end = reinterpret_cast<End>(dlsym(RTLD_DEFAULT, "markov_native_workspace_end"));
        end_ = end;
        if (begin != nullptr && end != nullptr) observation_ = begin(device, stream, requested_bytes, capacity_before, address_before, cached, release);

        // Worker queues do not inherit ATen's thread-local profiler callback.
        // Use the same clock, event schema and reporting path as native ranges.
        if (observation_ != nullptr && torch_npu::profiler::ProfilerMgr::GetInstance()->ReportEnable().load()) {
            using torch_npu::toolkit::profiler::Utils;
            range_ = std::make_unique<torch_npu::toolkit::profiler::OpRangeData>(Utils::GetClockTime(),
                                                                                 0,
                                                                                 -1,
                                                                                 Utils::GetPid(),
                                                                                 Utils::GetTid(),
                                                                                 0,
                                                                                 0,
                                                                                 false,
                                                                                 release ? "MarkovWorkspaceRelease" : "MarkovWorkspaceAllocate");
            // Upload and UploadWithLock have separate buffered file writers.
            // A dedicated file prevents their TLV records from interleaving.
            range_->tag = "markov.workspace";
        }
    }

    ~WorkspaceObservation() {
        // An exception leaves the resulting allocator state unknown. It must
        // not be reported as a successful transition to zero capacity.
        if (observation_ != nullptr) {
            finish_range();
            end_(observation_, 0, nullptr, -1);
        }
    }

    WorkspaceObservation(const WorkspaceObservation &) = delete;
    WorkspaceObservation & operator=(const WorkspaceObservation &) = delete;

    void finish(std::uint64_t capacity_after, const void * address_after) {
        if (observation_ != nullptr) {
            finish_range();
            end_(observation_, capacity_after, address_after, 0);
            observation_ = nullptr;
        }
    }

private:
    void finish_range() {
        if (!range_) return;

        using torch_npu::toolkit::profiler::Utils;
        range_->end_ns = Utils::GetClockTime();
        range_->end_thread_id = Utils::GetTid();
        auto * profiler = torch_npu::profiler::ProfilerMgr::GetInstance();
        if (profiler->ReportEnable().load()) profiler->UploadWithLock(std::move(range_));
    }

    using Begin = void * (*)(int, void *, std::uint64_t, std::uint64_t, const void *, bool, bool);
    using End = void (*)(void *, std::uint64_t, const void *, int);

    void * observation_ = nullptr;
    End end_ = nullptr;
    std::unique_ptr<torch_npu::toolkit::profiler::OpRangeData> range_;
};

} // namespace markov::collection
