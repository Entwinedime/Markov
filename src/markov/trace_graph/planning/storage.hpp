#pragma once

#include "markov/trace_graph/planning/cache.hpp"
#include "markov/trace_graph/planning/program.hpp"
#include "markov/trace_graph/planning/storage_config.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace markov::trace_graph::planning {

// Request queues preserve native worker ownership across multiple service
// fragments. They choose work; all resource time still belongs to ProgramRuntime.
class Storage {
public:
    static constexpr simulation::Component owner = 1;

    Storage(Cache & cache, const TokenPaths & paths, std::size_t page_tokens, std::size_t bytes_per_token, std::size_t participants, std::size_t resource_begin,
            std::size_t requests, StorageConfig config, StorageBackend backend);
    Storage(const Storage &) = delete;

    // Issue once at enqueue; admission checks consume only the progress already
    // published by the worker. Completion alone cannot insert a host prefix.
    void begin_prefetch(ProgramRuntime & runtime, std::size_t request, std::span<const PrefixId> lookup, const CacheMatch & matched,
                        std::optional<graph::NodeId> cause);
    bool check_prefetch(ProgramRuntime & runtime, std::size_t request, std::optional<graph::NodeId> cause);
    void enqueue_write(ProgramRuntime & runtime, RadixId source, std::vector<PrefixId> pages, graph::NodeId cause);
    bool on_event(ProgramRuntime & runtime, const simulation::Event & event);
    bool idle() const { return prefetches_.empty() && writes_.empty(); }
    const std::array<std::optional<graph::NodeId>, 3> & completion_frontier() const { return completion_frontier_; }
    const std::vector<PrefetchResult> & results() const { return results_; }
    const StorageStats & stats() const { return stats_; }

private:
    enum class ActionKind : std::uint32_t { Allocated, PageRead, PageCopied, ReadReturned, Accept, Timeout, Gathered, Prepared, PageWritten, PageTouched };
    static simulation::Action action(ActionKind kind, std::size_t subject) { return { std::to_underlying(kind), subject }; }

    struct ReadCursor {
        std::size_t batch_begin = 0, batch_end = 0, read_index = 0, copy_index = 0;
        std::vector<bool> readable;
        std::vector<graph::NodeId> publications;
        bool terminated = false;
    };
    struct Prefetch {
        std::vector<PrefixId> prefix, keys;
        std::vector<ReadCursor> workers;
        std::vector<graph::NodeId> completed;
        BufferId buffer;
        RadixId anchor;
        std::optional<std::size_t> timer;
        std::optional<graph::NodeId> enqueue_cause;
        std::optional<graph::NodeId> publication_cause, deadline_cause;
        bool stopping = false;
    };
    struct Write {
        RadixId source;
        std::vector<PrefixId> pages;
        graph::NodeId enqueue_cause;
        std::vector<graph::NodeId> completed;
    };
    struct WriteCursor {
        std::size_t operation, index = 0, batch_end = 0;
    };
    struct Worker {
        std::map<PrefixId, std::size_t> files;
        std::deque<std::size_t> reads;
        std::deque<WriteCursor> writes;
        std::optional<graph::NodeId> read_completion, write_completion;
    };

    bool service(ProgramRuntime & runtime, const WorkKind & kind, std::size_t pages, std::size_t rank, bool writer, simulation::Action action,
                 std::optional<graph::NodeId> cause);
    graph::NodeId join(ProgramRuntime & runtime, std::span<const graph::NodeId> causes);
    std::optional<graph::NodeId> queued_cause(ProgramRuntime & runtime, std::optional<graph::NodeId> enqueue, std::optional<graph::NodeId> previous);
    void start_read(ProgramRuntime & runtime, std::size_t request, std::size_t rank, std::optional<graph::NodeId> cause);
    void read_page(ProgramRuntime & runtime, std::size_t request, std::size_t rank, graph::NodeId cause);
    void copy_page(ProgramRuntime & runtime, std::size_t request, std::size_t rank, graph::NodeId cause);
    void return_read(ProgramRuntime & runtime, std::size_t request, std::size_t rank, graph::NodeId cause);
    bool finish_read(ProgramRuntime & runtime, std::size_t request, std::size_t rank, graph::NodeId cause);
    void stop(ProgramRuntime & runtime, std::size_t request, std::optional<graph::NodeId> cause);
    bool accept(std::size_t request, graph::Time now_ns);
    void start_write(ProgramRuntime & runtime, std::size_t rank, std::optional<graph::NodeId> cause);
    bool write_page(ProgramRuntime & runtime, std::size_t rank, graph::NodeId cause);
    void touch(std::size_t rank, PrefixId key);
    void reserve_file(std::size_t rank);

    Cache & cache_;
    const TokenPaths & paths_;
    const std::size_t page_tokens_, page_bytes_, participants_, resource_begin_;
    const StorageConfig config_;
    const StorageBackend backend_;
    std::vector<PrefetchResult> results_;
    StorageStats stats_;
    std::vector<Worker> workers_;
    std::size_t access_ = 0, occupied_pages_ = 0, reserved_pages_ = 0;
    std::map<std::size_t, Prefetch> prefetches_;
    std::map<std::size_t, Write> writes_;

    // Read return, write completion and controller acceptance can finish apart.
    std::array<std::optional<graph::NodeId>, 3> completion_frontier_;
};

} // namespace markov::trace_graph::planning
