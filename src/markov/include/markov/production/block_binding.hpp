#pragma once

#include "markov/contracts/control.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace markov {

// Native layouts and wrapper metadata exist only while producing templates.
struct SourceTensorLayout {
    std::vector<std::int64_t> shape;
    std::vector<std::int64_t> stride;
    std::string dtype;

    static std::optional<SourceTensorLayout> read(const nlohmann::json & value);
};

struct SourceNativeRegion {
    std::string semantic;
    std::string module_type;
    std::string method;
    const nlohmann::json & result; // Immutable capture record, borrowed throughout template production.
    std::map<std::string, std::int64_t> parameters;
    std::map<std::string, std::int64_t> call_parameters;
    std::map<std::string, std::optional<SourceTensorLayout>> inputs;
    std::map<std::string, std::optional<SourceTensorLayout>> outputs;
    std::map<std::string, std::vector<std::int64_t>> cpu_values;
    std::optional<std::string> request_id = std::nullopt;
};

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
    SourceNativeRegion read(const nlohmann::json & record) const;
    std::string cpu_name() const;

    std::string_view identity;
    std::string_view semantic;
    std::string_view module_type;
    std::string_view method;
    std::span<const std::string_view> inputs{};
    std::span<const NativeRegionParameter> parameters{};
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
