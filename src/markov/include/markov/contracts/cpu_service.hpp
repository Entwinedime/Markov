#pragma once

#include "markov/contracts/batch_membership.hpp"

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
// Counts describe that work without retaining native request identities.
struct CpuScheduleWork {
    BatchTransitionState transition;
    std::size_t waiting_requests;
    std::vector<std::pair<std::size_t, std::size_t>> filters;
    std::optional<BatchMerge> merge;

    auto operator<=>(const CpuScheduleWork &) const = default;
    static CpuScheduleWork read(const nlohmann::json & value);
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

    auto operator<=>(const CpuServiceWork &) const = default;
    static CpuServiceWork from_input(CpuServiceDescriptor service, StepKind step, std::uint32_t rank, const BatchInput & input);
    static CpuServiceWork read(const nlohmann::json & value);
    nlohmann::json describe() const;
};

} // namespace markov
