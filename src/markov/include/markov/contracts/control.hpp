#pragma once

#include "markov/contracts/batch_membership.hpp"
#include "markov/contracts/request.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace markov {

using RequestId = std::size_t; // Index in the current immutable workload, not a display ID.
using WorkId = std::uint64_t;

enum class StepKind { Schedule, Prepare, Prefill, Decode, Publish };
enum class PortKind { Selected, Prepared, Submitted, DeviceDone, Published };
enum class BatchPhase { Prefill, Decode, Idle };

struct PortRef {
    WorkId work_id;
    PortKind port;
};

struct RequestSlice {
    RequestId request_id;
    std::uint32_t input_tokens;
    std::uint32_t context_tokens;
    std::uint32_t generated_tokens;      // Output tokens already published when this batch was formed.
    bool finishes;                       // Expected publication effect; committed only at the Published port.
    std::uint32_t pool_slot = 0;         // Nonzero logical reservation assigned before invocation; zero is padding.
    std::vector<std::uint32_t> kv_pages; // Logical target page identities reserved through this invocation.
};

// Describes work, not a second copy of the controller's request state.
struct BatchInput {
    BatchPhase phase;
    std::vector<RequestSlice> requests;
    std::uint64_t free_kv_pages;      // Free logical pages before Prepare, distinct from admission reservations.
    BatchTransitionState transition;  // Snapshot before filtering/adopting the previous prefill.
    AdmissionEntryState admission;    // Snapshot after transition, before candidate traversal.
    std::vector<BatchFilter> filters; // Ordered native batch selections, including no-op and empty results.
    std::optional<BatchMerge> merge = std::nullopt;
    AdmissionWork admission_work;
};

struct PortDependency {
    PortRef predecessor;
    PortKind target;
};

struct InvokeStep {
    WorkId work_id; // Assigned once by the controller; acceptance is not completion.
    StepKind step;
    BatchInput input;
    std::vector<PortRef> dependencies; // Entry gates, applied per rank.
    std::vector<PortDependency> port_dependencies;
};

struct CompleteRequest {
    RequestId request_id;
    PortRef published;
};

struct UnsupportedControl {
    std::string_view reason;
};

// An empty action list yields to the runtime. It does not busy-poll or charge time.
using ControlAction = std::variant<InvokeStep, CompleteRequest, UnsupportedControl>;

struct RequestArrived {
    RequestId request_id;
};

struct WorkPortCompleted {
    PortRef port;
};

struct ControlEvent {
    std::int64_t time_ns;
    std::variant<RequestArrived, WorkPortCompleted> payload;
};

// The runtime owns work status and time. Views live only through handle().
struct RuntimeView {
    std::int64_t time_ns;
    std::span<const WorkId> in_flight;
};

struct ControllerSummary {
    std::size_t completed_requests;
};

// Limits are interpreted by the production binding before entering control.
// They describe the supported cold, non-overlap policy, not native configuration.
struct ServingLimits {
    std::uint32_t request_capacity;
    std::uint32_t kv_capacity_tokens;
    std::uint32_t prefill_capacity_tokens;
};

struct AdmissionPolicy {
    double initial_ratio;
    double minimum_ratio;
    double decay_per_decode;
    std::uint32_t output_estimate_limit_tokens;
    std::uint32_t reserve_tokens_per_request;

    bool operator==(const AdmissionPolicy &) const = default;
};

class Controller {
public:
    virtual ~Controller() = default;

    virtual void initialize(std::span<const Request> requests, ServingLimits limits) = 0;

    // All events at time_ns are ordered and delivered together, before a new decision.
    virtual std::vector<ControlAction> handle(std::span<const ControlEvent> events, const RuntimeView & runtime) = 0;
    virtual ControllerSummary finish() const = 0;
};

enum class BlockParameterKind { RequestIds, BatchIdentity, Phase, RequestBatch, AdmissionPolicy, WorkerControls };

struct BlockParameter {
    std::string_view name;
    BlockParameterKind kind;
};

struct BlockDefinition {
    StepKind kind;
    std::string_view name;
    std::string_view entry;
    std::span<const PortKind> exits;
    std::string_view effect;
    std::span<const BlockParameter> parameters;
};

// The public plan, capture input and template readers share these block definitions.
class ServingContract {
public:
    std::span<const BlockDefinition> steps() const;
    const BlockDefinition & block(StepKind kind) const;
    const BlockDefinition & block(std::string_view name) const;

    nlohmann::json describe() const;
    std::string_view port_name(PortKind port) const;
    std::string_view phase_name(BatchPhase phase) const;
};

} // namespace markov
