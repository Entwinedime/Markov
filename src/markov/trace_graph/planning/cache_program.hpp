#pragma once

#include "markov/trace_graph/planning/cache.hpp"
#include "markov/trace_graph/planning/program.hpp"
#include "markov/trace_graph/source/context.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace markov::trace_graph::planning {

struct CacheBackend {
    std::size_t layers, bytes_per_token, participants, minimum_load_tokens;
    WorkKind load, backup;
};

struct CacheRequestResult {
    std::size_t device_hit_pages = 0, loaded_pages = 0, prefill_tokens = 0;
    bool completed = false;
};

// Finite two-tier cache and the known compute subset share one clock. Storage,
// CPU and cross-stream communication are not represented by this component.
class CacheProgram final : public Program {
public:
    CacheProgram(const source::SourceContext & source, std::size_t page_tokens, CacheConfig config, CacheBackend backend);
    void start(ProgramRuntime & runtime) override;
    void on_event(ProgramRuntime & runtime, const simulation::Event & event) override;
    bool complete() const override { return (issue_.has_value() || request_ == requests_.size()) && copies_.empty(); }

    std::size_t resources() const { return source_.observations().resources.size() + 2 * backend_.participants; }
    const std::vector<CacheRequestResult> & requests() const { return requests_; }
    const std::optional<std::string> & issue() const { return issue_; }
    std::size_t backups() const { return backups_; }
    std::size_t loads() const { return loads_; }
    std::size_t device_evictions() const { return device_evictions_; }
    std::size_t host_evictions() const { return host_evictions_; }
    std::size_t peak_device_pages() const { return peak_device_pages_; }
    std::size_t peak_host_pages() const { return peak_host_pages_; }

private:
    enum ActionKind { PhaseDone, CopyDone, EvictionDone };
    struct PendingCopy {
        RadixId node;
        bool evict_after;
        CopyKind kind;
        std::size_t pages;
        graph::NodeId completion;
    };

    void advance(ProgramRuntime & runtime);
    void compute(ProgramRuntime & runtime, Phase phase);
    void commit(ProgramRuntime & runtime);
    bool make_device_room(ProgramRuntime & runtime, std::size_t pages);
    bool make_host_room(std::size_t pages);
    bool copy(ProgramRuntime & runtime, TransferId transfer, RadixId node, CopyKind kind, std::size_t pages, bool evict_after);
    bool submit(ProgramRuntime & runtime, PlannedMutation mutation);
    void record_peak();

    const source::SourceContext & source_;
    const std::size_t page_tokens_;
    Cache cache_;
    const CacheBackend backend_;
    std::vector<CacheRequestResult> requests_;
    std::map<TransferId, PendingCopy> copies_;
    std::vector<RadixId> pending_evictions_;
    std::optional<BufferId> buffer_;
    std::optional<RadixId> request_pin_;
    std::optional<RadixId> host_pin_;
    std::optional<std::string> issue_;
    std::optional<graph::NodeId> cause_;
    std::size_t request_ = 0;
    std::size_t backups_ = 0, loads_ = 0, device_evictions_ = 0, host_evictions_ = 0;
    std::size_t peak_device_pages_ = 0, peak_host_pages_ = 0;
    Phase phase_ = Phase::Prefill;
    bool waiting_ = false;
};

} // namespace markov::trace_graph::planning
