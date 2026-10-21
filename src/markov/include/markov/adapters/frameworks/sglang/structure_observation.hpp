#pragma once

#include "markov/contracts/observation_semantics.hpp"
#include "markov/production/source_call.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace markov {

class AscendSourceExtractor;
class SGLangBindings;

class SGLangStructureObservation final : public ObservationSemantics {
public:
    SGLangStructureObservation(const AscendSourceExtractor & source, const SGLangBindings & bindings, std::uint32_t request_capacity);

    nlohmann::json scope(std::size_t call) const override;
    nlohmann::json compare_state(std::size_t call, const nlohmann::json & invocation, const nlohmann::json & resource_nodes) const override;
    std::vector<ObservedCollective> collectives(std::size_t call) const override;
    std::vector<ObservedCollectiveImplementation> collective_implementations(std::size_t call) const override;
    std::optional<ObservedBatchOutput> output(std::size_t call) const override;
    std::optional<std::vector<std::pair<std::size_t, BoundaryId>>> incoming_request_ready(std::size_t call) const override;
    std::span<const ObservedCpuService> cpu_services(std::size_t call) const override;
    std::optional<DeviceCommandWork> command(SourceSpanId span) const override;
    nlohmann::json coverage() const override;

private:
    std::size_t serving_call(std::size_t call) const;
    void read_collectives();
    void read_input_messages();
    void read_cpu_services();
    nlohmann::json compare_workspace(std::size_t call, const nlohmann::json & resource_nodes) const;

    const AscendSourceExtractor & source_;
    const SGLangBindings & bindings_;
    std::uint32_t request_capacity_;
    std::vector<std::size_t> serving_calls_;
    std::vector<std::vector<std::size_t>> regions_;
    std::vector<std::vector<ObservedCollective>> collectives_;
    std::vector<std::vector<ObservedCollectiveImplementation>> collective_implementations_;
    std::vector<std::vector<ObservedCpuService>> cpu_services_;
    nlohmann::json cpu_coverage_;
    std::vector<std::vector<std::size_t>> workspace_uses_;
    std::map<std::pair<std::uint32_t, std::uint64_t>, const nlohmann::json *> workspace_windows_;
    std::map<nlohmann::json, std::size_t> workspace_last_;
    nlohmann::json collective_gaps_{
        {    "total",                       0 },
        {    "shown",                       0 },
        { "examples", nlohmann::json::array() }
    };
    // Comparison-only alpha-renaming for matched actions in execution order.
    // Native preparation and publication remain the owners of slot lifetime.
    mutable std::map<std::pair<std::uint32_t, std::string>, std::pair<std::uint32_t, std::uint32_t>> live_slots_;
    // Pool identities are alpha-renamed by matched semantic uses, not native IDs.
    mutable std::map<nlohmann::json, nlohmann::json> workspace_pools_;
};

} // namespace markov
