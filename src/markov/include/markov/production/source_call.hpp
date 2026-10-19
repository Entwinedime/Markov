#pragma once

#include "markov/contracts/control.hpp"
#include "markov/contracts/source_observations.hpp"
#include "markov/graph/source_graph.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace markov {

struct SourcePublication {
    std::string request_id;
    bool finished;
};

struct SourceOutputPolicy {
    std::uint32_t force_interval_tokens;
    bool non_streaming;
};

struct SourceAdmissionPolicy {
    AdmissionPolicy parameters;
    double entry_ratio;

    static SourceAdmissionPolicy read(const nlohmann::json & value);
};

struct SourceWorkerControls {
    double waiting_timeout_seconds;
    double running_timeout_seconds;
    bool test_retract;
    bool require_mlp_sync;

    static SourceWorkerControls read(const nlohmann::json & value);
};

struct SourceBatchFilter {
    std::vector<std::string> before;
    std::vector<std::string> after;
};

enum class SourceCallKind { Serving, ForwardInputs, NativeRegion };

struct SourceCall {
    std::string identity;
    StepKind step;
    SourceCallKind kind;
    std::uint32_t rank;
    std::optional<std::size_t> parent;

    std::optional<std::uint64_t> batch_id;
    std::optional<BatchPhase> phase;
    std::vector<SourceRequest> requests;

    std::optional<SourceSpanId> marker;
    std::int64_t observed_start_ns; // Native monotonic entry, only for ordering source observations.
    std::optional<BoundaryId> device_done;

    std::vector<SourcePublication> publications;
    std::optional<std::string> binding;
    std::vector<SourceBatchFilter> batch_filters;
    nlohmann::json module = nullptr; // Opaque observation; interpreted once by the template producer.
    std::optional<SourceOutputPolicy> output_policy = std::nullopt;
    std::optional<SourceAdmissionPolicy> admission_policy = std::nullopt;
    std::optional<SourceWorkerControls> worker_controls = std::nullopt;

    bool is_annotation() const { return kind != SourceCallKind::Serving; }

    // Observation nesting must not change the enclosing serving action.
    std::optional<std::size_t> serving_parent(std::span<const SourceCall> calls) const {
        auto ancestor = parent;
        while (ancestor && calls[*ancestor].is_annotation()) ancestor = calls[*ancestor].parent;
        return ancestor;
    }
};

} // namespace markov
