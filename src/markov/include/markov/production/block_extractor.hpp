#pragma once

#include "markov/production/source_call.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace markov {

class SourceBindings;

// The source extractor owns these observations for the complete extraction call.
// Framework adapters resolve native ports; ownership and graph assembly are shared.
class BlockExtractor {
public:
    BlockExtractor(const SourceGraph & graph, std::span<const SourceCall> calls, const std::vector<std::vector<std::size_t>> & native_api_members,
                   bool native_dense_path, const SourceBindings & bindings);
    virtual ~BlockExtractor() = default;

    nlohmann::json extract() const;

protected:
    virtual nlohmann::json resolve_ports(std::size_t call_index) const = 0;

    const SourceGraph & graph_;
    std::span<const SourceCall> calls_;

private:
    nlohmann::json extract_regions() const;
    const std::vector<std::vector<std::size_t>> & native_api_members_;
    bool native_dense_path_;
    const SourceBindings & bindings_;
};


} // namespace markov
