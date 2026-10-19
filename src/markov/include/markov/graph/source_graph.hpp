#pragma once

#include "markov/contracts/batch_membership.hpp"
#include "markov/contracts/cpu_service.hpp"

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
#include <variant>
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
    std::optional<std::size_t> source_api; // Supplementary provenance; hardware payload facts do not require an SDK hook.

    nlohmann::json describe() const;
    static SourceTransfer read(const nlohmann::json & value);
    static std::string_view direction_name(TransferDirection direction);
    static TransferDirection parse_direction(std::string_view name);
};
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

    nlohmann::json describe() const;
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

enum class RequestPoolOperation { PrefixExtendWrite, TokenRead, TokenWrite, RowRead, RelayRead, RelayWrite };

struct RequestPoolRange {
    std::uint32_t slot;
    std::int64_t prefix_tokens;
    std::int64_t input_tokens;

    auto operator<=>(const RequestPoolRange &) const = default;
};

struct RequestPoolPoint {
    std::uint32_t slot;
    std::int64_t column;

    auto operator<=>(const RequestPoolPoint &) const = default;
};

// Access roles come from the framework producer; the core retains their actual
// rows and token ranges without interpreting kernel names or device values.
struct RequestPoolWork {
    RequestPoolOperation operation;
    std::variant<std::vector<std::uint32_t>, std::vector<RequestPoolPoint>, std::vector<RequestPoolRange>> members;

    static RequestPoolWork read(const nlohmann::json & value);
    static std::string_view operation_name(RequestPoolOperation operation);
    nlohmann::json describe() const;
    auto operator<=>(const RequestPoolWork &) const = default;
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
// raw observations leave derived workload metadata absent.
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
    std::optional<BatchFilter> batch_filter = std::nullopt;
    std::optional<RequestPoolWork> request_pool = std::nullopt;
    std::optional<AttentionWorkload> attention = std::nullopt; // Target semantic parameters, absent in raw kernel observations.
    // Value-dependent work without a measured operand contract. Keep its identity;
    // full request/KV state remains on the invocation instead of each kernel.
    std::vector<std::string> value_dependencies = {};

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
    std::size_t lane(SourceDomain domain, std::uint32_t rank, std::uint64_t native_id);
    SourceSpanId observe(SourceSpanKind kind, std::size_t lane, std::int64_t begin_ns, std::int64_t end_ns, std::optional<std::size_t> owner_call,
                         std::optional<std::size_t> source_interval, std::optional<std::size_t> source_api = std::nullopt,
                         std::optional<std::size_t> scope_call = std::nullopt);
    void observe_kernel(SourceSpanId span, SourceKernel kernel);
    void observe_transfer(SourceSpanId span, SourceTransfer transfer);
    void observe_stream_wait(SourceSpanId span, SourceStreamWait wait);
    void observe_operation(SourceOperation operation);
    void observe_cpu_boundary(BoundaryId boundary, CpuBoundary description);
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
    const std::map<SourceSpanId, SourceOperation> & operations() const;
    const std::map<SourceSpanId, SourceCollective> & collectives() const;
    const std::vector<SourceEdge> & edges() const;
    const std::vector<SourceLane> & lanes() const;
    const std::vector<SourceBoundary> & boundaries() const;
    const std::map<SourceSpanId, SourceKernel> & kernels() const { return kernels_; }
    const std::map<SourceSpanId, SourceTransfer> & transfers() const { return transfers_; }
    const std::map<SourceSpanId, SourceStreamWait> & stream_waits() const { return stream_waits_; }
    const std::map<BoundaryId, CpuBoundary> & cpu_boundaries() const { return cpu_boundaries_; }

    void write(const std::filesystem::path & path) const;

private:
    BoundaryId boundary(std::size_t lane, std::int64_t time_ns);

    std::vector<SourceLane> lanes_;
    std::vector<SourceBoundary> boundaries_;
    std::vector<SourceSpan> spans_;
    std::map<SourceSpanId, SourceKernel> kernels_;
    std::map<SourceSpanId, SourceTransfer> transfers_;
    std::map<SourceSpanId, SourceStreamWait> stream_waits_;
    std::map<SourceSpanId, SourceOperation> operations_;
    std::map<BoundaryId, CpuBoundary> cpu_boundaries_;
    std::map<SourceSpanId, SourceCollective> collectives_;
    std::vector<SourceEdge> edges_;

    std::map<std::tuple<SourceDomain, std::uint32_t, std::uint64_t>, std::size_t> lane_ids_;
    std::map<std::pair<std::size_t, std::int64_t>, BoundaryId> boundary_ids_;
};

} // namespace markov
