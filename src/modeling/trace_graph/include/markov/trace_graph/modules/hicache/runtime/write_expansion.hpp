#pragma once
#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_host.hpp"
#include "markov/trace_graph/modules/hicache/runtime/writes.hpp"
#include "markov/trace_graph/simulation/cpu_task_queues.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace markov::trace_graph::cost {
struct HostOperationCost;
}

namespace markov::trace_graph::modules::hicache::runtime {

/** Remember new work inserted between the same pair of original stream nodes,
 * including insertions made at different CPU callbacks. */
class HiCacheWriteStreamInsertions {
public:
    [[nodiscard]] HiCacheWriteStreamPosition position(size_t lane, HiCacheWriteStreamPosition source) const;
    void advance(size_t lane, HiCacheWriteStreamPosition source, size_t tail);
    /** Capture resource consumers before any source calls are removed. */
    void bind_synchronizations(const core::DagGraph & graph);
    void constrain_synchronizations(size_t lane, const HiCacheWriteStreamPosition & source, size_t tail, simulation::FutureDag & future) const;

private:
    // Nested callbacks insert inside a copied call while its outer stream cut
    // still owns the call's last device node. Neither cut may erase the other.
    using Cut = std::tuple<size_t, std::optional<size_t>, std::optional<size_t>>;
    std::map<Cut, size_t> tails_;
    const core::DagGraph * graph_ = nullptr;
    std::map<size_t, std::vector<size_t>> synchronizations_;
};

/** Local execution graph, generated from target work or copied from retained
 * source work, without observed arrival/completion times. */
struct HiCacheHostExpansion {
    struct Node {
        core::DagSyntheticNodeSpec work;
        std::optional<size_t> submission, queue_member;
        // Local previous member of the same task; never a second FIFO job.
        std::optional<size_t> task_predecessor;
        // Additional state callbacks inside an otherwise private program.
        bool observe_start = false;
    };
    struct Edge {
        size_t from, to;
        core::DagEdgeKind kind;
    };
    struct Stream {
        size_t source_node, first, last;
    };
    struct Wait {
        size_t source_node, consumer;
    };
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::vector<Stream> streams;
    std::vector<Wait> waits;
    // Specific event completions must be supplied explicitly by the caller;
    // unlike stream barriers they cannot use the current lane frontier.
    std::vector<Wait> event_waits;
    // Existing collective completion nodes outside this replacement. These
    // must remain active until expansion; unlike a stream frontier, a later
    // operation on the same lane cannot substitute for this exact event.
    std::vector<Wait> collective_completions;
    std::map<size_t, size_t> source_nodes;
    size_t host_return = 0;

    /** Append zero-cost caller work on the supplied lane, chain it after the
     * local previous node and update that cursor. Returns a local slot ID;
     * no worker task, device wait or graph mutation is implied. */
    size_t append_main(std::string_view lane, std::string name, std::optional<size_t> & previous, std::string category = "hicache_patch");

    /** In expand_host position order: emitted streams, then stream barriers.
     * Repeated resources remain repeated; event waits are bound separately. */
    [[nodiscard]] std::vector<size_t> resource_nodes() const;
    [[nodiscard]] std::vector<size_t> resource_lanes(const core::DagGraph & graph) const;
};

struct HiCacheWriteExpansion : HiCacheHostExpansion {
    std::map<size_t, uint64_t> payload;
    size_t write_start = 0, completion = 0;
    uint64_t payload_bytes = 0;
};

/** Generate CPU work and its return without a source template or costs. */
[[nodiscard]] HiCacheHostExpansion generate_cpu_control(std::string_view lane, std::string_view name);

/** Bind independent service to a generate_cpu_control result. Residual time
 * stays distinct and completes before the return. Only cost/provenance change;
 * fractional microseconds are carried by the caller across related calls. */
void bind_cpu_control(HiCacheHostExpansion & plan, double cpu_us, double residual_us, std::pair<double, double> & remainders);

/** Observe surviving worker lanes once after all source replacements.
 * Values are queue-member node IDs, not task costs or dispatch estimates. */
[[nodiscard]] std::map<size_t, size_t> observe_host_worker_members(const core::DagGraph & graph);
/** Refresh insertion endpoints disabled by another source-region replacement.
 * Empty positions describe target-only streams and remain empty. */
void rebind_host_stream_position(const adapters::sglang::HiCacheSourceIndex & source, HiCacheWriteStreamPosition & position, const std::string & pid,
                                 const std::string & tid, uint64_t at_us);
void rebind_host_stream_positions(const adapters::sglang::HiCacheSourceIndex & source, std::map<size_t, HiCacheWriteStreamPosition> & positions,
                                  const std::string & pid, const std::string & tid, uint64_t at_us);
/** Rebind copied tasks using the final source graph's exact worker lanes. */
void rebind_host_worker_queues(const core::DagGraph & graph, const std::map<size_t, size_t> & members, std::span<HiCacheHostExpansion *> plans);

/** Semantic resources observed inside a write call, independent of numeric
 * lane ids. Worker roles are identified by the calls submitting their tasks. */
[[nodiscard]] std::map<std::string, size_t> write_resource_roles(const core::DagGraph & graph, const HiCacheWriteExpansion & plan, size_t compute_record);

/** Host work with optional explicitly identified compute stream. Worker identities
 * come from original submission calls, not generated names or calibration ids. */
[[nodiscard]] std::map<std::string, size_t> host_resource_roles(const core::DagGraph & graph, const HiCacheHostExpansion & plan, size_t main_node,
                                                                std::optional<size_t> compute_node = {});

/** Copy retained source work, optionally including caller-owned zero-cost
 * callback points inside the same interval. These points retain a source_nodes
 * correspondence so the caller can move nested execution callbacks after
 * expansion; they do not imply migration of the callback's modeled work.
 * Dispatch, device launches and queue release remain explicit. Optional
 * compaction joins only private serial CPU segments within the same task.
 * retain_collective_completions opts into exact external HCCL dependencies;
 * its caller must hand them over during removal and preserve their source
 * nodes through expansion, including when generating different tensor work.
 */
struct HostExpansionOptions {
    std::span<const size_t> callback_points;
    bool retain_collective_completions = false;
    bool compact = false;
};

[[nodiscard]] HiCacheHostExpansion prepare_host_expansion(const adapters::sglang::HiCacheSourceIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                                          const HiCacheHostTemplate & host,
                                                          std::string_view prefix = "target control: ", HostExpansionOptions options = {});
[[nodiscard]] HiCacheWriteExpansion prepare_write_expansion(const adapters::sglang::HiCacheSourceIndex & source,
                                                            const simulation::detail::CpuTaskQueues & queues, const patch::HiCacheIoOperationRecord & write,
                                                            const HiCacheHostTemplate & host, size_t completion);

struct HiCacheExpandedWrite {
    size_t host_return, write_start, completion;
    std::vector<size_t> stream_tails;
    // Per-layer readiness needs intermediate Records, not just completion.
    std::vector<size_t> nodes;
};

struct HiCacheExpandedHost {
    size_t host_return;
    std::vector<size_t> stream_tails, nodes;
};

[[nodiscard]] HiCacheExpandedHost expand_host(const HiCacheHostExpansion & plan, std::span<const HiCacheWriteStreamPosition> positions,
                                              simulation::FutureDag & future, std::optional<size_t> previous_host_return = std::nullopt,
                                              std::span<const size_t> event_completions = {});

/** Distribute independent payload service across declared byte-weighted slots.
 * Prefix rounding preserves total microseconds. Only duration/provenance are
 * changed; resource selection, transfers and completion dependencies are fixed.
 */
void bind_transfer_payload(HiCacheWriteExpansion & plan, uint64_t service_us);

/** Emit an already cost-bound plan. Caller resolves each stream at the new
 * CPU insertion point; emission does not estimate or redistribute costs. */
[[nodiscard]] HiCacheExpandedWrite expand_write(const HiCacheWriteExpansion & plan, std::span<const HiCacheWriteStreamPosition> positions,
                                                simulation::FutureDag & future, std::optional<size_t> previous_host_return = std::nullopt);

/** One CPU insertion callback can emit several calls on shared device streams.
 * Each append continues the preceding host call and advances touched streams. */
class HiCacheHostSequence {
public:
    HiCacheHostSequence(const std::map<size_t, HiCacheWriteStreamPosition> & positions, HiCacheWriteStreamInsertions & insertions,
                        simulation::FutureDag & future, std::optional<size_t> previous_host_return = std::nullopt);
    HiCacheExpandedHost append(const HiCacheHostExpansion & plan, std::span<const size_t> lanes, std::span<const size_t> event_completions = {});
    HiCacheExpandedWrite append(const HiCacheWriteExpansion & plan, std::span<const size_t> lanes);

private:
    std::vector<HiCacheWriteStreamPosition> resolve(std::span<const size_t> lanes) const;
    void advance(std::span<const size_t> lanes, std::span<const size_t> tails, size_t host_return);
    const std::map<size_t, HiCacheWriteStreamPosition> & original_;
    HiCacheWriteStreamInsertions & insertions_;
    simulation::FutureDag & future_;
    std::optional<size_t> host_return_;
};

} // namespace markov::trace_graph::modules::hicache::runtime
