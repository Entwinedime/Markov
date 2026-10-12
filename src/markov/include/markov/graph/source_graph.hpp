#pragma once

#include "markov/contracts/batch_membership.hpp"

#include <nlohmann/json_fwd.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace markov {

using BoundaryId = std::size_t;
using SourceSpanId = std::size_t;

enum class SourceDomain { CpuThread, DeviceStream, CommunicationObservation, LogicalCompletion, DeviceTaskCompletion };
enum class SourceSpanKind { CpuObservation, CpuControl, HostWait, Compute, Dma, DeviceWait, DeviceMarker, CommunicationObservation };

enum class TransferDirection { HostToHost, HostToDevice, DeviceToHost, DeviceToDevice };

struct SourceTransfer {
    std::uint64_t bytes;
    TransferDirection direction;
    std::size_t source_api;

    nlohmann::json describe() const;
    static SourceTransfer read(const nlohmann::json & value);
};
enum class SourceCpuPart { SchedulePrefix, PrepareBody, PrepareSuffix, Execute, Publish, Dispatch };
enum class SourceRelation {
    IntervalExtent,
    ThreadOrder,
    StreamOrder,
    NativeFlow,
    NativeNotifySignal,
    NativeEventSignal,
    NativeHostWait,
    NativeTaskDependency,
    CollectiveSubmission,
    CompletionJoin,
    ModelDataDependency
};

struct SourceLane {
    SourceDomain domain;
    std::uint32_t rank;
    std::uint64_t native_id;
};

struct SourceBoundary {
    std::size_t lane;
    std::int64_t observed_time_ns;
    std::optional<std::size_t> owner_call; // Ownership of derived logical gates; CPU annotations may have several users.
};

// A source span preserves an observation. Its elapsed wall time is not a service
// cost: CPU observations can nest, and device/host waits require separate causes.
struct SourceSpan {
    SourceSpanKind kind;
    std::optional<std::size_t> owner_call;
    std::optional<std::size_t> source_interval;
    BoundaryId begin;
    BoundaryId end;
    std::optional<SourceCpuPart> control_part;
    std::optional<std::size_t> source_api; // Native host waits use this instead of a profiler source_interval.
    std::optional<std::size_t> scope_call; // Innermost observed call; owner_call remains the serving action.
};

struct SourceEdge {
    BoundaryId source;
    BoundaryId target;
    SourceRelation relation;
    std::optional<std::size_t> source_reference; // Flow, Notify_Record interval, or native API index, according to relation.
};

enum class SynchronizationScope { Stream, Device };

struct SourceStreamWait {
    std::optional<std::uint64_t> native_stream;
    std::vector<std::size_t> lanes; // Empty means an unqualified mapping, not an observed idle stream.
    SynchronizationScope scope = SynchronizationScope::Stream;
};

enum class AttentionOperation { CacheWrite, Prefill, Decode };

struct KvSequence {
    std::int64_t input_tokens;
    std::int64_t prefix_tokens;
    std::uint32_t request_pool_slot;
    std::vector<std::uint32_t> kv_pages;

    auto operator<=>(const KvSequence &) const = default;

    nlohmann::json describe_addresses(std::int64_t page_size, std::int64_t batch_offset, bool include_writes) const;
};

// Derived from the target invocation, not from cache capacity or source timing.
struct AttentionWorkload {
    AttentionOperation operation;
    std::vector<KvSequence> sequences;
    std::int64_t query_heads;
    std::int64_t kv_heads;
    std::int64_t head_dim;
    std::int64_t page_size_tokens;
    std::int64_t cache_pages; // Observed allocated layout, including padding; not the live KV length.
    auto operator<=>(const AttentionWorkload &) const = default;

    nlohmann::json describe() const;
};

struct KvAllocationWorkload {
    std::int64_t page_size_tokens;
    std::uint64_t free_pages_before;
    std::vector<KvSequence> sequences;

    nlohmann::json describe() const;
};

struct PageTableWorkload {
    std::int64_t page_size_tokens;
    std::vector<KvSequence> sequences;

    nlohmann::json describe() const;
};

struct KvReleaseWorkload {
    std::int64_t page_size_tokens;
    std::uint64_t free_pages_before;
    std::vector<KvSequence> sequences;

    nlohmann::json describe() const;
};

struct PositionSequence {
    std::int64_t prefix_tokens;
    std::int64_t input_tokens;
};

// Compact description of the target position tensor; no per-token allocation.
struct PositionWorkload {
    std::vector<PositionSequence> sequences;

    nlohmann::json describe() const;
};

// Vendor tensor text is retained exactly, including empty optional slots.
// Template instances transform copies and may attach derived workload metadata;
// raw observations do not carry target attention parameters.
struct SourceKernel {
    std::string name;
    std::string operation;
    std::string input_shapes;
    std::string output_shapes;
    std::string input_dtypes;
    std::string output_dtypes;
    std::string input_formats;
    std::string output_formats;
    std::optional<std::vector<std::int64_t>> logits_token_indices = std::nullopt;
    std::optional<PositionWorkload> positions = std::nullopt;
    std::optional<KvAllocationWorkload> kv_allocation = std::nullopt;
    std::optional<KvReleaseWorkload> kv_release = std::nullopt;
    std::optional<BatchFilter> batch_filter = std::nullopt;
    std::optional<BatchMerge> batch_merge = std::nullopt;
    std::optional<PageTableWorkload> page_table = std::nullopt;
    std::optional<std::int64_t> sequence_length_max = std::nullopt;
    std::optional<std::vector<std::int64_t>> sequence_lengths = std::nullopt;
    std::optional<std::int64_t> kv_new_pages = std::nullopt;
    std::optional<std::vector<std::uint32_t>> request_pool_slots = std::nullopt;
    std::optional<AttentionWorkload> attention = std::nullopt; // Target semantic parameters, absent in raw kernel observations.

    nlohmann::json describe() const;
};

struct SourceOperation {
    std::string name;
    SourceSpanId host_span;
    std::vector<SourceSpanId> worker_spans;
    std::vector<SourceSpanId> device_spans;
    std::optional<std::string> input_shapes = std::nullopt;
    std::optional<std::string> input_types = std::nullopt;
    std::vector<SourceSpanId> collective_spans = {};
};

// Collective envelopes describe logical work; physical services retain their own
// spans and dependencies. Keeping both must never imply charging both.
struct SourceCommunicationTask {
    SourceSpanId span;
    std::size_t source_detail;
    std::uint64_t stream;
    std::uint64_t task;
    std::uint64_t context;
    std::string operation;
    std::uint64_t bytes;
    std::string data_type;
    std::string transport;
    std::uint32_t source_rank;
    std::uint32_t destination_rank;

    nlohmann::json describe() const;
};

struct SourceTransport {
    std::string operation;
    std::uint32_t peer;
    std::string transport;
};

struct SourceCommunicator {
    std::uint64_t buffer_bytes;
    std::uint32_t deterministic;
    std::uint32_t op_expansion_mode;
    std::uint32_t rank_count;
    std::uint32_t server_count;
    std::uint32_t module_count;
    std::optional<std::string> device_type;
    std::optional<std::string> hcomm_version;
    std::optional<std::uint32_t> env_aicpu_unfold;
    std::optional<std::uint32_t> env_aiv_mode;
    std::vector<SourceTransport> transports;

    nlohmann::json describe() const;
    static SourceCommunicator read(const nlohmann::json & value);
};

struct SourceCollective {
    SourceSpanId span;
    std::uint32_t rank;
    std::uint64_t connection;
    std::string name;
    std::uint64_t count;
    std::string data_type;
    std::string algorithm;
    std::string group_name;
    std::optional<SourceCommunicator> communicator;
    std::vector<SourceCommunicationTask> tasks;

    nlohmann::json describe() const;
    static SourceCollective read(const nlohmann::json & value);
};

struct SourceGraphSummary {
    std::size_t boundaries;
    std::size_t spans;
    std::size_t edges;
    std::size_t visited_boundaries;
    std::size_t flow_time_reversals;
    std::size_t notify_signals;
    std::size_t event_signals;
    std::size_t host_wait_dependencies;
};

// CPU annotations at (native thread, timestamp) share a coordinate. CANN's
// synthetic process ID is absent because Python and CANN share that thread.
// Device/communication endpoints retain distinct identities even at equal times;
// timestamps alone cannot prove dependency or merge parallel sub-tasks.
class SourceGraph {
public:
    static std::string_view span_name(SourceSpanKind kind);
    static std::string_view relation_name(SourceRelation relation);
    static SourceRelation parse_relation(std::string_view name);
    static SourceDomain parse_domain(std::string_view name);
    std::size_t lane(SourceDomain domain, std::uint32_t rank, std::uint64_t native_id);
    SourceSpanId observe(SourceSpanKind kind, std::size_t lane, std::int64_t begin_ns, std::int64_t end_ns, std::optional<std::size_t> owner_call,
                         std::optional<std::size_t> source_interval, std::optional<SourceCpuPart> control_part = std::nullopt,
                         std::optional<std::size_t> source_api = std::nullopt, std::optional<std::size_t> scope_call = std::nullopt);
    void observe_kernel(SourceSpanId span, SourceKernel kernel);
    void observe_transfer(SourceSpanId span, SourceTransfer transfer);
    void observe_stream_wait(SourceSpanId span, SourceStreamWait wait);
    void observe_operation(SourceOperation operation);
    void observe_collective(SourceCollective collective);
    void connect(BoundaryId source, BoundaryId target, SourceRelation relation, std::optional<std::size_t> source_reference = std::nullopt);

    // A pure completion boundary joins the observed device ends. Its source
    // coordinate is descriptive; the future runtime must compute target time.
    BoundaryId complete(std::span<const SourceSpanId> spans, std::uint32_t rank, std::uint64_t group, SourceDomain domain = SourceDomain::LogicalCompletion);

    // Called once after observation and flow ingestion. It adds CPU thread
    // ordering and audits cycles. The adapter supplies native device task order;
    // overlapping hardware sub-tasks cannot be serialized by their timestamps.
    SourceGraphSummary seal();

    const std::vector<SourceSpan> & spans() const;
    const std::vector<SourceCollective> & collectives() const;
    const std::vector<SourceEdge> & edges() const;
    const std::vector<SourceBoundary> & boundaries() const;

    void write(const std::filesystem::path & path) const;

private:
    BoundaryId boundary(std::size_t lane, std::int64_t time_ns);

    std::vector<SourceLane> lanes_;
    std::vector<SourceBoundary> boundaries_;
    std::vector<SourceSpan> spans_;
    std::map<SourceSpanId, SourceKernel> kernels_;
    std::map<SourceSpanId, SourceTransfer> transfers_;
    std::map<SourceSpanId, SourceStreamWait> stream_waits_;
    std::vector<SourceOperation> operations_;
    std::vector<SourceCollective> collectives_;
    std::vector<SourceEdge> edges_;

    std::map<std::tuple<SourceDomain, std::uint32_t, std::uint64_t>, std::size_t> lane_ids_;
    std::map<std::pair<std::size_t, std::int64_t>, BoundaryId> boundary_ids_;
};

} // namespace markov
