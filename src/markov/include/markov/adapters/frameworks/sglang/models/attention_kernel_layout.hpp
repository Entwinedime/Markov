#pragma once

#include "markov/contracts/control.hpp"
#include "markov/graph/source_graph.hpp"
#include "markov/templates/attention_parameters.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace markov {

struct SourceNativeRegion;
struct SourceRequest;

// The observed Ascend ND paged-cache paths. Shape relations and logical
// attention work do not certify a new launch decomposition or backend branch.
class AttentionKernelLayout {
public:
    static std::optional<AttentionKernelLayout> bind(const SourceNativeRegion & region, const SourceKernel & kernel, StepKind step,
                                                     std::span<const SourceRequest> requests, std::uint32_t page_size_tokens);
    AttentionParameterRule compile() const;

private:
    AttentionKernelLayout(AttentionOperation operation, std::int64_t query_heads, std::int64_t kv_heads, std::int64_t head_dim, std::int64_t page_size_tokens,
                          std::int64_t cache_pages);

    AttentionOperation operation_;
    std::int64_t query_heads_;
    std::int64_t kv_heads_;
    std::int64_t head_dim_;
    std::int64_t page_size_tokens_;
    std::int64_t cache_pages_;
};

} // namespace markov
