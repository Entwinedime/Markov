#pragma once

#include "markov/contracts/control.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace markov {

struct SourceNativeRegion;

enum class NativePortAnchor { ScheduleDecision, ParentScheduleReturn, ScopeReturn, OwnedDeviceCompletion };
enum class NativeWrapperKind { Schedule, Prepare, Execute, Publish };

struct BlockPortBinding {
    PortKind port;
    NativePortAnchor anchor;
};

struct BlockBinding {
    std::string_view identity;
    StepKind step;
    std::optional<BatchPhase> phase;
    std::string_view hook;
    NativeWrapperKind wrapper;
    std::span<const BlockPortBinding> ports;
};

struct NativeRegionParameter {
    std::string_view name;
    std::string_view attribute;
};

struct NativeRegionOutput {
    std::string_view name;
    std::optional<std::size_t> index;
};

struct NativeRegionBinding {
    SourceNativeRegion read(const nlohmann::json & metadata) const;

    std::string_view identity;
    std::string_view semantic;
    std::string_view module_type;
    std::string_view method;
    std::span<const std::string_view> inputs;
    std::span<const NativeRegionParameter> parameters;
    std::span<const NativeRegionParameter> call_parameters{};
    std::span<const NativeRegionOutput> outputs{};
    std::span<const NativeRegionParameter> module_inputs{};
    std::string_view component_path{};
    std::span<const std::string_view> cpu_inputs{};
    std::string_view component_owner = "batch";
    std::string_view request_id_path{};
    std::string_view hook{};
};


} // namespace markov
