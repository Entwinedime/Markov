#pragma once
#include "markov/trace_graph/modules/hicache/runtime/write_host.hpp"
#include "markov/trace_graph/modules/hicache/runtime/writes.hpp"
#include "markov/trace_graph/simulation/cpu_task_queues.hpp"

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
    struct Entry {
        HiCacheWriteStreamPosition source;
        size_t tail;
    };
    std::map<size_t, Entry> tails_;
    const core::DagGraph * graph_ = nullptr;
    std::map<size_t, std::vector<size_t>> synchronizations_;
};

/** A donor's small execution graph, with no donor arrival/completion times. */
struct HiCacheHostExpansion {
    struct Node {
        core::DagSyntheticNodeSpec work;
        std::optional<size_t> submission, queue_member;
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
    std::map<size_t, size_t> source_nodes;
    size_t host_return = 0;
};

struct HiCacheWriteExpansion : HiCacheHostExpansion {
    std::map<size_t, uint64_t> payload;
    size_t write_start = 0, completion = 0;
    uint64_t payload_bytes = 0;
};

/** Generate pure CPU work and its return, with independently supplied cost.
 * Residual service remains distinct and must finish before the return node.
 * This constructor never requires a source execution template. */
[[nodiscard]] HiCacheHostExpansion generated_cpu_control(std::string_view lane, double cpu_us, double residual_us,
                                                         std::pair<double, double> & remainders, std::string_view name);

/** Ordered target event waits. Costs are independent inputs; event identities
 * are bound by expand_host, never copied from a source check. Per-event service
 * is aggregated after completion; this is not a measured latency upper bound. */
[[nodiscard]] HiCacheHostExpansion generated_completion_check(std::string_view lane, size_t events, double entry_cpu_us, double entry_residual_us,
    double event_cpu_us, double event_residual_us, std::pair<double, double> & remainders);

/** Generate target FAST2D K/V page count/bytes, retaining preparation and tail.
 * CPU per-page costs are a constant-cost extrapolation across page widths;
 * growth needs a measured steady page, not another target execution template. */
[[nodiscard]] HiCacheWriteExpansion resize_write_pages(const HiCacheWriteExpansion & plan, uint64_t target_bytes, uint64_t page_bytes);
/** Candidate geometry check; detailed dependency closure is checked on resize. */
[[nodiscard]] bool write_page_geometry_matches(const HiCacheWriteExpansion & plan, uint64_t target_bytes, uint64_t page_bytes);

/** Pure CPU call; retain fractional microseconds across calls on this thread. */
[[nodiscard]] HiCacheHostExpansion calibrated_cpu_control(std::string_view lane, double cost_us, double & remainder_us, std::string_view name);

/** Rebind copied tasks after source-call removal, using surviving tasks on the
 * exact original worker lane. No queue or ready-delay estimate is invented. */
void rebind_host_worker_queues(const core::DagGraph & graph, std::span<HiCacheHostExpansion *> plans);

/** Semantic resources observed inside a write call, independent of numeric
 * lane ids. Worker roles are identified by the calls submitting their tasks. */
[[nodiscard]] std::map<std::string, size_t> write_resource_roles(
    const core::DagGraph & graph, const HiCacheWriteExpansion & plan, size_t compute_record);

/** Host work with optional explicitly identified compute stream. Worker identities
 * come from original submission calls, not generated names or calibration ids. */
[[nodiscard]] std::map<std::string, size_t> host_resource_roles(
    const core::DagGraph & graph, const HiCacheHostExpansion & plan, size_t main_node,
    std::optional<size_t> compute_node = {});

/** Resource identity only, witnessed by the same main lane's real submissions. */
[[nodiscard]] std::optional<size_t> find_host_worker_resource(
    const core::DagGraph & graph, const simulation::detail::CpuTaskQueues & queues,
    size_t main_node, std::string_view submission_name);

[[nodiscard]] HiCacheHostExpansion prepare_host_expansion(const patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                                          const HiCacheHostTemplate & host, std::string_view prefix = "target control: ");
[[nodiscard]] HiCacheWriteExpansion prepare_write_expansion(const patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                                            const patch::HiCacheIoOperationRecord & write, const HiCacheHostTemplate & host, size_t completion);

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

/** Caller resolves each stream at the new CPU insertion point before replay. */
[[nodiscard]] HiCacheExpandedWrite expand_write(const HiCacheWriteExpansion & plan, uint64_t service_us, std::span<const HiCacheWriteStreamPosition> positions,
                                                simulation::FutureDag & future, std::optional<size_t> previous_host_return = std::nullopt);

/** One CPU insertion callback can emit several calls on shared device streams.
 * Each append continues the preceding host call and advances touched streams. */
class HiCacheHostSequence {
public:
    HiCacheHostSequence(const std::map<size_t, HiCacheWriteStreamPosition> & positions, HiCacheWriteStreamInsertions & insertions,
                        simulation::FutureDag & future, std::optional<size_t> previous_host_return = std::nullopt);
    HiCacheExpandedHost append(const HiCacheHostExpansion & plan, std::span<const size_t> lanes, std::span<const size_t> event_completions = {});
    HiCacheExpandedWrite append(const HiCacheWriteExpansion & plan, uint64_t service_us, std::span<const size_t> lanes);

private:
    std::vector<HiCacheWriteStreamPosition> resolve(std::span<const size_t> lanes) const;
    void advance(std::span<const size_t> lanes, std::span<const size_t> tails, size_t host_return);
    const std::map<size_t, HiCacheWriteStreamPosition> & original_;
    HiCacheWriteStreamInsertions & insertions_;
    simulation::FutureDag & future_;
    std::optional<size_t> host_return_;
};

} // namespace markov::trace_graph::modules::hicache::runtime
