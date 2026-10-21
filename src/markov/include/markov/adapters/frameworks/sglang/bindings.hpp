#pragma once

#include "markov/adapters/frameworks/sglang/configuration.hpp"
#include "markov/contracts/batch_membership.hpp"
#include "markov/production/block_binding.hpp"
#include "markov/production/source_bindings.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace markov {

class SourceGraph;

// Native locations and their extraction meaning are declared together.
class SGLangBindings final : public SourceBindings {
public:
    enum class WorkerControlQualification { Supported, NotObserved, Unsupported };

    std::expected<ServingLimits, std::string_view> control_limits(const SGLangConfiguration & configuration) const;
    std::uint32_t request_capacity(const nlohmann::json & environment) const;
    std::optional<AdmissionStop> admission_stop(const SourceNativeRegion & region, std::uint32_t request_capacity) const;
    WorkerControlQualification qualify_worker_controls(const nlohmann::json & metadata) const;
    bool supports_native_path(const nlohmann::json & environment, std::uint32_t physical_tp_size) const;
    bool supports_configuration_change(const SGLangConfiguration & source, SGLangConfiguration target) const;
    std::span<const BlockBinding> bindings() const;
    const BlockBinding & binding(std::string_view identity) const;
    nlohmann::json describe() const;
    std::vector<NativeRegionBinding> regions() const;
    const NativeRegionBinding & region(std::string_view identity) const;
    std::map<PortKind, BoundaryId> ports(const SourceGraph & graph, std::span<const SourceCall> calls, std::size_t call_index) const override;
    std::optional<std::string> cpu_name(const SourceCall & call) const override;
    bool cpu_work_boundary(const SourceCall & call) const override;
    nlohmann::json describe_regions() const;

private:
    std::span<const NativeRegionBinding> serving_regions() const;
};


} // namespace markov
