#pragma once

#include "markov/contracts/batch_membership.hpp"
#include "markov/contracts/collective_work.hpp"

#include <nlohmann/json_fwd.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace markov {

enum class StepKind;
enum class BatchPhase;
struct BatchInput;

enum class CpuThreadRole { Control, Worker };
enum class CpuBoundaryKind { Scope, Function, Enqueue, Worker, Runtime, Wait, Transfer, InternalWait };
enum class CpuBoundaryEdge { Begin, End };

// Names describe observed implementation ranges. Native IDs and clocks are
// provenance; queue position is relative to the enclosing operation only.
struct CpuBoundary {
    CpuThreadRole role;
    CpuBoundaryKind kind;
    CpuBoundaryEdge edge;
    std::string name;
    std::size_t ordinal = 0;
    std::size_t count = 1;

    auto operator<=>(const CpuBoundary &) const = default;
    static CpuBoundary read(const nlohmann::json & value);
    nlohmann::json describe() const;
    static std::string_view role_name(CpuThreadRole role);
    static std::string_view kind_name(CpuBoundaryKind kind);
};

struct CpuServiceDescriptor {
    CpuBoundary begin;
    CpuBoundary end;

    auto operator<=>(const CpuServiceDescriptor &) const = default;
    static CpuServiceDescriptor read(const nlohmann::json & value);
    nlohmann::json describe() const;
};

// Scheduling can scan and filter members that never enter the selected batch.
// Counts and ordered incoming lengths describe that work without retaining
// native request identities or substituting the selected model batch.
struct CpuScheduleWork {
    BatchTransitionState transition;
    std::size_t waiting_requests;
    std::vector<std::pair<std::size_t, std::size_t>> filters;
    std::optional<BatchMerge> merge;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> incoming_requests;

    auto operator<=>(const CpuScheduleWork &) const = default;
    static CpuScheduleWork read(const nlohmann::json & value);
    nlohmann::json describe() const;
};

// Publish scans the ordered batch even when no member emits. Token counts
// describe output materialization, without retaining payloads or mutable offsets.
struct CpuPublicationWork {
    struct Request {
        std::uint64_t output_tokens;
        std::uint64_t decode_context_tokens;
        std::uint64_t history_tokens; // Full terminal-history slice width, not a sum over native reads.

        auto operator<=>(const Request &) const = default;
    };
    std::vector<Request> requests;

    auto operator<=>(const CpuPublicationWork &) const = default;
    static CpuPublicationWork read(const nlohmann::json & value);
    nlohmann::json describe() const;
};

// Target work is resolved by execution before querying a model. Per-request
// identities, source timing and controller objects never enter this value.
struct CpuServiceWork {
    CpuServiceDescriptor service;
    StepKind step;
    BatchPhase phase;
    std::uint32_t rank;
    std::uint64_t requests;
    std::uint64_t input_tokens;
    std::uint64_t context_tokens;
    std::uint64_t finished_requests;
    std::optional<CpuScheduleWork> scheduling = std::nullopt;
    std::optional<CollectiveWork> collective = std::nullopt;
    std::optional<CpuPublicationWork> publication = std::nullopt;

    auto operator<=>(const CpuServiceWork &) const = default;
    static CpuServiceWork from_input(CpuServiceDescriptor service, StepKind step, std::uint32_t rank, const BatchInput & input,
                                     std::optional<CpuPublicationWork> publication);
    static CpuServiceWork read(const nlohmann::json & value);
    nlohmann::json describe() const;
};

} // namespace markov
