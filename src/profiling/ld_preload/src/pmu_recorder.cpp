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

/** @brief PAPI 错误统一输出到 stderr，避免污染 Chrome trace JSON。 */
void LogPapiError(const std::string & action, int error_code) { std::cerr << "[hook] " << action << " failed: " << PAPI_strerror(error_code) << std::endl; }

/** @brief PAPI 线程 id 使用 Linux tid，和 trace event 的 tid 字段对齐。 */
unsigned long PapiThreadId() { return static_cast<unsigned long>(syscall(SYS_gettid)); }

/** @brief 每个线程独立持有 PAPI event set，避免跨线程复用计数器状态。 */
struct PapiThreadState {
    bool registered{false};
    bool initialized{false};
    int event_set{PAPI_NULL};
    std::vector<std::string> event_names;
    std::vector<long long> counter_values;

    void Cleanup() {
        /**
         * @brief 清理顺序保持 stop -> cleanup -> destroy -> unregister，避免 PAPI 资源泄漏到目标进程。
         */
        if (event_set != PAPI_NULL) {
            long long * stop_values{counter_values.empty() ? nullptr : counter_values.data()};
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

/** @brief PAPI-backed PMU recorder 实现；未启用或初始化失败时保持静默降级。 */
class PmuRecorderImpl {
  public:
    PmuRecorderImpl() {
        /**
         * @brief PMU 由编译开关和环境变量共同控制，默认事件只作为采集事实附加到 trace。
         */
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
        /**
         * @brief PAPI library 和 thread support 必须同时初始化成功，后续线程快照才可用。
         */
        int ret{PAPI_library_init(PAPI_VER_CURRENT)};
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
        /**
         * @brief 从 HOOK_PAPI_EVENTS 读取事件列表；非法事件被跳过而不是中断基础 hook。
         */
        const std::string default_events{"perf::CYCLES,perf::INSTRUCTIONS,perf::CACHE-"
                                         "REFERENCES,perf::CACHE-MISSES"};
        const std::string env_events{safe_env("HOOK_PAPI_EVENTS")};

        const std::vector<std::string> requested_events{SplitCsv(env_events.empty() ? default_events : env_events)};
        if (requested_events.empty()) {
            std::cerr << "[hook] No PAPI events configured, PMU trace disabled" << std::endl;
            return;
        }

        for (size_t i = 0; i < requested_events.size(); ++i) {
            int event_code{0};
            int ret{PAPI_event_name_to_code(const_cast<char *>(requested_events[i].c_str()), &event_code)};
            if (ret != PAPI_OK) {
                std::cerr << "[hook] Unsupported PAPI event: " << requested_events[i] << " (" << PAPI_strerror(ret) << ")" << std::endl;
                continue;
            }

            event_names_.push_back(requested_events[i]);
            event_codes_.push_back(event_code);
        }
    }

    bool InitializeThreadState(PapiThreadState & state) {
        /**
         * @brief 线程首次读取 PMU 时惰性创建 event set。
         *
         * 目标服务线程可能很多，按线程懒初始化能避免 hook 加载阶段为未使用线程创建资源。
         */
        if (state.initialized) { return true; }

        int ret{PAPI_register_thread()};
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

/** @brief 未启用 PAPI 编译开关时的空实现，保证基础 trace 采集不依赖 PMU。 */
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
