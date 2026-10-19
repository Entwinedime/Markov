#pragma once

#include "markov/contracts/control.hpp"
#include "markov/graph/source_graph.hpp"
#include "markov/templates/batch_parameters.hpp"
#include "markov/templates/request_pool_parameters.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <string>

namespace markov {

struct SourceRequest;

struct IndexedWriteSource {
    BatchOperationVariant variant;
    SourceSpanId write_span;
    const SourceKernel * write; // Bank-owned immutable observation.
    std::optional<SourceSpanId> broadcast_span;
};

// Fixed Ascend dense path: resize the request table, views and FutureMap relay, not
// arbitrary tensors whose dimensions happen to contain the same integer.
class RequestPoolLayout {
public:
    RequestPoolLayout(std::uint32_t source_capacity, const std::map<SourceSpanId, SourceKernel> & kernels,
                      const std::map<SourceSpanId, SourceOperation> & operations);
    const IndexedWriteSource * indexed_write(SourceSpanId operation) const;
    RequestPoolCapacityDomain capacity_domain() const;
    std::optional<std::int64_t> sequence_capacity() const;
    std::expected<std::optional<RequestPoolParameterLayout>, std::string> bind(SourceSpanId source_span, const SourceKernel & source,
                                                                               std::span<const SourceRequest> source_requests) const;

private:
    std::expected<RequestPoolParameterLayout, std::string> bind_broadcast(const SourceKernel & broadcast, const IndexedWriteSource & binding,
                                                                          std::span<const SourceRequest> source_requests) const;

    std::map<SourceSpanId, IndexedWriteSource> indexed_writes_;
    std::map<SourceSpanId, SourceSpanId> broadcasts_; // Device span -> owning operation.
    std::uint32_t source_capacity_;
    std::optional<std::array<std::int64_t, 2>> table_;
};

} // namespace markov
