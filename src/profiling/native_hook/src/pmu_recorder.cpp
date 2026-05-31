#include "framework/pmu_recorder.hpp"
#include "framework/common.hpp"
#if defined(HOOK_ENABLE_PAPI)
#include <papi.h>
#endif
#include <iostream>
#include <sys/syscall.h>
#include <unistd.h>

namespace HookFrameWork {

namespace {

#if defined(HOOK_ENABLE_PAPI)

void LogPapiError(const std::string & action, int error_code) { std::cerr << "[hook] " << action << " failed: " << PAPI_strerror(error_code) << std::endl; }

unsigned long PapiThreadId() { return static_cast<unsigned long>(syscall(SYS_gettid)); }

struct PapiThreadState {
    bool registered{ false };
    bool initialized{ false };
    int event_set{ PAPI_NULL };
    std::vector<std::string> event_names;
    std::vector<long long> counter_values;

    void Cleanup() {
        if (event_set != PAPI_NULL) {
            long long * stop_values{ counter_values.empty() ? nullptr : counter_values.data() };
            PAPI_stop(event_set, stop_values);
            PAPI_cleanup_eventset(event_set);
            PAPI_destroy_eventset(&event_set);
            event_set = PAPI_NULL;
        }

        if (registered) {
            PAPI_unregister_thread();
            registered = false;
        }

        initialized = false;
        event_names.clear();
        counter_values.clear();
    }

    ~PapiThreadState() { Cleanup(); }
};

class PmuRecorderImpl {
public:
    PmuRecorderImpl() {
        if (!ParseEnvFlag("HOOK_ENABLE_PAPI", true)) { return; }
        if (!InitializeLibrary()) { return; }

        LoadRequestedEvents();
        if (event_codes_.empty()) {
            std::cerr << "[hook] All configured PAPI events are invalid, PMU trace disabled" << std::endl;
            return;
        }

        enabled_ = true;
        std::cerr << "[hook] PMU enabled with " << event_codes_.size() << " PAPI events" << std::endl;
    }

    bool ReadSnapshot(PmuSnapshot * snapshot) {
        if (!enabled_ || snapshot == nullptr) { return false; }

        static thread_local PapiThreadState thread_state;
        if (!InitializeThreadState(thread_state)) { return false; }

        int ret = PAPI_read(thread_state.event_set, thread_state.counter_values.data());
        if (ret != PAPI_OK) {
            LogPapiError("PAPI_read", ret);
            return false;
        }

        snapshot->valid = true;
        snapshot->event_names = thread_state.event_names;
        snapshot->counter_values = thread_state.counter_values;
        return true;
    }

private:
    bool InitializeLibrary() {
        int ret{ PAPI_library_init(PAPI_VER_CURRENT) };
        if (ret != PAPI_VER_CURRENT) {
            std::cerr << "[hook] PAPI_library_init failed" << std::endl;
            return false;
        }

        ret = PAPI_thread_init(PapiThreadId);
        if (ret != PAPI_OK) {
            LogPapiError("PAPI_thread_init", ret);
            return false;
        }

        return true;
    }

    void LoadRequestedEvents() {
        const std::string default_events{ "perf::CYCLES,perf::INSTRUCTIONS,perf::CACHE-"
                                          "REFERENCES,perf::CACHE-MISSES" };
        const std::string env_events{ safe_env("HOOK_PAPI_EVENTS") };

        const std::vector<std::string> requested_events{ SplitCsv(env_events.empty() ? default_events : env_events) };
        if (requested_events.empty()) {
            std::cerr << "[hook] No PAPI events configured, PMU trace disabled" << std::endl;
            return;
        }

        for (size_t i = 0; i < requested_events.size(); ++i) {
            int event_code{ 0 };
            int ret{ PAPI_event_name_to_code(const_cast<char *>(requested_events[i].c_str()), &event_code) };
            if (ret != PAPI_OK) {
                std::cerr << "[hook] Unsupported PAPI event: " << requested_events[i] << " (" << PAPI_strerror(ret) << ")" << std::endl;
                continue;
            }

            event_names_.push_back(requested_events[i]);
            event_codes_.push_back(event_code);
        }
    }

    bool InitializeThreadState(PapiThreadState & state) {
        if (state.initialized) { return true; }

        int ret{ PAPI_register_thread() };
        if (ret != PAPI_OK) {
            LogPapiError("PAPI_register_thread", ret);
            return false;
        }
        state.registered = true;

        state.event_set = PAPI_NULL;
        ret = PAPI_create_eventset(&state.event_set);
        if (ret != PAPI_OK) {
            LogPapiError("PAPI_create_eventset", ret);
            state.Cleanup();
            return false;
        }

        for (size_t i = 0; i < event_codes_.size(); ++i) {
            ret = PAPI_add_event(state.event_set, event_codes_[i]);
            if (ret != PAPI_OK) {
                std::cerr << "[hook] PAPI_add_event failed for " << event_names_[i] << ": " << PAPI_strerror(ret) << std::endl;
                continue;
            }
            state.event_names.push_back(event_names_[i]);
        }

        if (state.event_names.empty()) {
            std::cerr << "[hook] No available PMU events on this thread" << std::endl;
            state.Cleanup();
            return false;
        }

        state.counter_values.assign(state.event_names.size(), 0);
        ret = PAPI_start(state.event_set);
        if (ret != PAPI_OK) {
            LogPapiError("PAPI_start", ret);
            state.Cleanup();
            return false;
        }

        state.initialized = true;
        return true;
    }

    bool enabled_ = false;
    std::vector<std::string> event_names_;
    std::vector<int> event_codes_;
};

#else

class PmuRecorderImpl {
public:
    bool ReadSnapshot(PmuSnapshot *) { return false; }
};

#endif

PmuRecorderImpl & GetPmuRecorderImpl() {
    static PmuRecorderImpl impl;
    return impl;
}

} // namespace

PmuRecorder & PmuRecorder::Get() {
    static PmuRecorder instance;
    return instance;
}

bool PmuRecorder::ReadSnapshot(PmuSnapshot * snapshot) { return GetPmuRecorderImpl().ReadSnapshot(snapshot); }

} // namespace HookFrameWork
