#pragma once

#include "markov/trace_graph/planning/cache.hpp"
#include "markov/trace_graph/planning/compute_program.hpp"
#include "markov/trace_graph/planning/program.hpp"
#include "markov/trace_graph/planning/storage.hpp"
#include "markov/trace_graph/source/context.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace markov::trace_graph::planning {

struct CacheBackend {
    std::size_t layers, bytes_per_token, participants, minimum_load_tokens;
    WorkKind load, backup;
    StorageBackend storage;
};

struct CacheRequestResult {
    std::size_t device_hit_pages = 0, loaded_pages = 0, prefill_tokens = 0;
    std::size_t deferred_kv_pages = 0;
    std::optional<std::size_t> device_pages_allocated_since_return;
    bool completed = false;
    std::optional<graph::NodeId> start, arrived, received, prefill_prefix_published, response, kv_published, work_completed;
};

// Cache/storage, known compute and blocking collectives share one clock.
// Native cache/release control and complete cross-stream dependencies remain outside this component.
class CacheProgram final : public Program, public ResultControls {
public:
    CacheProgram(const source::SourceContext & source, std::size_t page_tokens, CacheConfig config, CacheBackend backend,
                 std::optional<StorageConfig> storage = std::nullopt, std::optional<KernelCacheInitial> kernel_cache_initial = std::nullopt);

    // Storage borrows this cache, so the program must remain at one address.
    CacheProgram(const CacheProgram &) = delete;
    void start(ProgramRuntime & runtime) override;
    void on_event(ProgramRuntime & runtime, const simulation::Event & event) override;
    bool complete() const override;

    std::size_t resources() const { return compute_resource_begin() + source_.observations().collective_groups.size(); }
    ComputeStats compute_stats() const;
    const Storage * storage() const { return storage_ ? &*storage_ : nullptr; }
    const std::vector<CacheRequestResult> & requests() const { return requests_; }
    const std::optional<std::string> & issue() const { return issue_; }
    const PreparationCache & preparations() const { return preparations_; }
    std::size_t backups() const { return backups_; }
    std::size_t loads() const { return loads_; }
    std::size_t device_evictions() const { return device_evictions_; }
    std::size_t host_evictions() const { return host_evictions_ + (storage_ ? storage_->stats().host_evictions : 0); }
    std::size_t peak_device_pages() const { return cache_.peak_pages(Tier::Device); }
    std::size_t peak_host_pages() const { return cache_.peak_pages(Tier::Host); }

private:
    enum class ActionKind : std::uint32_t {
        CopyDone,
        EvictionDone,
        Received,
        AdmissionGranted,
        Admitted,
        ResponseDone,
        PrefixInserted,
        FinalInserted,
        FinalPublished
    };
    enum class RequestStage { BeforeStart, Ingress, Waiting, Admitting, Active, Response };
    static simulation::Action action(ActionKind kind, std::size_t subject) { return { std::to_underlying(kind), subject }; }

    struct PendingCopy {
        RadixId node;
        bool evict_after;
        CopyKind kind;
        std::size_t pages;
        graph::NodeId completion;
        std::vector<PrefixId> storage_pages;
    };
    struct RunningRequest {
        struct Insertion {
            RadixId leaf;
            std::vector<Work> work;
            std::vector<graph::NodeId> completions;
            std::optional<graph::NodeId> completed;
        };
        std::unique_ptr<ComputeProgram> compute;
        std::optional<BufferId> buffer;
        std::optional<RadixId> pin, committed;
        std::optional<PublicationId> prefix_publication, final_publication;
        std::vector<std::size_t> callers;
        std::vector<graph::NodeId> admission;
        std::optional<Insertion> prefix_insertion, final_insertion;
        std::optional<graph::NodeId> outputs_ready;
        bool publication_scheduled = false;
    };

    std::vector<Work> begin_result_control(std::size_t request, ResultControl control) override;
    void end_result_control(ProgramRuntime & runtime, std::size_t request, ResultControl control, graph::NodeId completion) override;
    void complete_insertion(ProgramRuntime & runtime, std::size_t request, ResultControl control);
    void advance(ProgramRuntime & runtime);
    void begin_request(ProgramRuntime & runtime, bool drain);
    void wait_for_progress(ProgramRuntime & runtime);
    void respond(ProgramRuntime & runtime, std::size_t request);
    void compute(ProgramRuntime & runtime);
    void publish_prefill_prefix(ProgramRuntime & runtime, std::size_t request);
    void backup_prefix(ProgramRuntime & runtime, RadixId leaf);
    void commit(ProgramRuntime & runtime, std::size_t request);
    void publish_result(ProgramRuntime & runtime, std::size_t request);
    bool make_device_room(ProgramRuntime & runtime, std::size_t pages);
    bool make_host_room(std::size_t pages);
    bool copy(ProgramRuntime & runtime, TransferId transfer, RadixId node, CopyKind kind, std::size_t pages, bool evict_after);
    bool submit(ProgramRuntime & runtime, PlannedMutation mutation);
    std::size_t compute_resource_begin() const { return source_.observations().resources.size() + (storage_ ? 4 : 2) * backend_.participants; }

    const source::SourceContext & source_;
    const std::size_t page_tokens_;
    Cache cache_;
    const CacheBackend backend_;
    PreparationCache preparations_;
    std::optional<Storage> storage_;
    ExecutionQueues queues_;
    std::vector<RunningRequest> running_;
    ComputeStats compute_stats_;
    std::vector<CacheRequestResult> requests_;
    std::map<TransferId, PendingCopy> copies_;
    std::array<std::optional<graph::NodeId>, 2> copy_frontier_;
    std::vector<RadixId> pending_evictions_;
    std::optional<RadixId> host_pin_;
    std::optional<std::string> issue_;
    std::optional<graph::NodeId> cause_;
    std::size_t request_ = 0;
    std::size_t backups_ = 0, loads_ = 0, device_evictions_ = 0, host_evictions_ = 0;
    RequestStage request_stage_ = RequestStage::BeforeStart;
    bool waiting_ = false;
    bool eviction_wait_ = false;
};

} // namespace markov::trace_graph::planning
