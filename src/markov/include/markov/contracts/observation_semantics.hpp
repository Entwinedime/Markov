#pragma once

#include "markov/contracts/service_work.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace markov {

// A native logical operation and its observed structural interface. Each
// frontier is an AND gate (the maximum of its observed points). Device entries
// join the actual transport input and submission cuts; completions stay rank
// local. CPU protocol, synchronization and output layout remain ordinary work. These gates do not relocate early physical work or
// qualify a transferable performance cost. Empty gates never mean completion.
struct ObservedCollective {
    std::size_t call;
    SourceSpanId source_collective;
    CollectiveWork work;
    nlohmann::json scope;
    BoundaryId host_begin;
    BoundaryId host_end;
    std::vector<BoundaryId> entries;
    std::vector<BoundaryId> completions;
    // Protocol-derived constraints between observed native cuts, separate
    // from the service entry frontier and from raw transport observations.
    std::vector<std::pair<BoundaryId, BoundaryId>> completion_entry_dependencies = {};
};

// Ownership is observed independently of group/layout qualification. An
// unqualified implementation is unavailable logical work, not extra model
// kernels or commands, and never grants a comparable collective interface.
// Qualification concerns structural projection only, not a latency model.
struct ObservedCollectiveImplementation {
    std::size_t call;
    std::vector<SourceSpanId> spans;
    bool qualified = false;
};

struct ObservedRequestOutput {
    std::string request_id;
    std::uint64_t tokens;
    std::uint64_t cumulative_tokens;
    bool finishes;
};

struct ObservedBatchOutput {
    nlohmann::json scope;
    BoundaryId begin;
    BoundaryId end;
    std::vector<ObservedRequestOutput> requests;
};

// A named service on one actual native CPU lane. Endpoint scopes may differ:
// private scope changes do not split an otherwise continuous service.
struct ObservedCpuService {
    CpuServiceDescriptor service;
    BoundaryId begin;
    BoundaryId end;
    std::size_t native_lane;
    nlohmann::json begin_scope;
    nlohmann::json end_scope;
};

// Interprets native observation facts without introducing a second controller
// or executable template source. The comparison borrows this object.
class ObservationSemantics {
public:
    virtual ~ObservationSemantics() = default;

    virtual nlohmann::json scope(std::size_t call) const = 0;
    // Resource observations carry actual target effects, not additional work.
    virtual nlohmann::json compare_state(std::size_t call, const nlohmann::json & invocation, const nlohmann::json & resource_nodes) const = 0;
    virtual std::vector<ObservedCollective> collectives(std::size_t call) const = 0;
    virtual std::vector<ObservedCollectiveImplementation> collective_implementations(std::size_t call) const = 0;
    virtual std::optional<ObservedBatchOutput> output(std::size_t call) const = 0;
    // Unknown reception differs from an observed empty raw cohort. These are
    // actual handler returns, indexed independently of the selected batch.
    virtual std::optional<std::vector<std::pair<std::size_t, BoundaryId>>> incoming_request_ready(std::size_t call) const = 0;
    virtual std::span<const ObservedCpuService> cpu_services(std::size_t call) const = 0;
    virtual std::optional<DeviceCommandWork> command(SourceSpanId span) const = 0;
    virtual nlohmann::json coverage() const = 0;
};

} // namespace markov
