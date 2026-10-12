#pragma once

#include "markov/production/block_binding.hpp"
#include "markov/production/source_bindings.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace markov {

class SourceGraph;

// Native locations and their extraction meaning are declared together.
class SGLangBindings final : public SourceBindings {
public:
    std::size_t bind_transfers(SourceGraph & graph, const nlohmann::json & intervals, const nlohmann::json & records,
                               std::span<const SourceCall> calls) const override;
    std::expected<ServingLimits, std::string_view> control_limits(const SGLangConfiguration & configuration) const;
    bool supports_native_path(const nlohmann::json & environment) const override;
    bool supports_configuration_change(const SGLangConfiguration & source, SGLangConfiguration target) const;
    std::span<const BlockBinding> bindings() const;
    const BlockBinding & binding(std::string_view identity) const;
    nlohmann::json describe() const;
    std::vector<NativeRegionBinding> regions() const;
    const NativeRegionBinding & region(std::string_view identity) const override;
    nlohmann::json extract_blocks(const SourceGraph & graph, std::span<const SourceCall> calls,
                                  const std::vector<std::vector<std::size_t>> & native_api_members, bool native_dense_path) const override;
    nlohmann::json describe_regions() const;

private:
    std::span<const NativeRegionBinding> serving_regions() const;
};


} // namespace markov
