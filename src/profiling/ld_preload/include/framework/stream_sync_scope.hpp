#pragma once

namespace HookFrameWork {

// ACL may call an intercepted runtime entry for the same stream. Record one
// envelope, while keeping distinct streams and threads independently visible.
class StreamSyncScope {
public:
    explicit StreamSyncScope(void* stream) : stream_(stream), previous_(current_) {
        for (auto* scope = previous_; scope; scope = scope->previous_) {
            if (scope->stream_ == stream) nested_ = true;
        }
        current_ = this;
    }
    ~StreamSyncScope() { current_ = previous_; }
    StreamSyncScope(const StreamSyncScope&) = delete;
    StreamSyncScope& operator=(const StreamSyncScope&) = delete;
    bool Nested() const { return nested_; }

private:
    inline static thread_local StreamSyncScope* current_ = nullptr;
    void* stream_;
    StreamSyncScope* previous_;
    bool nested_ = false;
};

} // namespace HookFrameWork
