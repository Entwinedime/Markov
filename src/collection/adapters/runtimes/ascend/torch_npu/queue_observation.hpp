#pragma once

#include <cstdint>
#include <dlfcn.h>

#include "torch_npu/csrc/core/npu/interface/AsyncTaskQueueInterface.h"

namespace markov::collection {

// Included only in NPUQueue.cpp. Native task layouts never cross the C ABI.
class QueueObservation {
public:
    QueueObservation(const c10_npu::queue::QueueParas & task, bool worker) {
        static const auto begin = reinterpret_cast<Begin>(dlsym(RTLD_DEFAULT, "markov_native_queue_begin"));
        static const auto end = reinterpret_cast<End>(dlsym(RTLD_DEFAULT, "markov_native_queue_end"));
        end_ = end;
        if (begin == nullptr || end == nullptr) return;

        observation_ = begin(task.correlation_id, worker);
    }

    ~QueueObservation() {
        // An exception or early queue failure must not become a successful call.
        if (observation_ != nullptr) end_(observation_, -1);
    }

    QueueObservation(const QueueObservation &) = delete;
    QueueObservation & operator=(const QueueObservation &) = delete;

    void finish(int result) {
        if (observation_ == nullptr) return;
        end_(observation_, result);
        observation_ = nullptr;
    }

private:
    using Begin = void * (*)(std::uint64_t, int);
    using End = void (*)(void *, int);

    void * observation_ = nullptr;
    End end_ = nullptr;
};

} // namespace markov::collection
