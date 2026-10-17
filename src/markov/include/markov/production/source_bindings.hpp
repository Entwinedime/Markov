#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace markov {

class SourceGraph;
struct SourceCall;
struct NativeRegionBinding;

// Framework interpretation is injected into runtime-specific source graph extraction.
class SourceBindings {
public:
    virtual ~SourceBindings() = default;

    virtual const NativeRegionBinding & region(std::string_view identity) const = 0;
    virtual bool supports_native_path(const nlohmann::json & environment) const = 0;
    virtual nlohmann::json extract_blocks(const SourceGraph & graph, std::span<const SourceCall> calls,
                                          const std::vector<std::vector<std::size_t>> & native_api_members, bool native_dense_path) const = 0;
};

} // namespace markov
