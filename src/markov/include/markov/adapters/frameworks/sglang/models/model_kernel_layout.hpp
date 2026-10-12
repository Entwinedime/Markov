#pragma once

#include "markov/contracts/control.hpp"
#include "markov/graph/source_graph.hpp"
#include "markov/templates/model_parameters.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace markov {

struct SourceNativeRegion;
struct SourceRequest;
class KernelShapes;

// A shape relation with native module provenance. It does not qualify kernel
// selection, launch decomposition, or the surrounding graph for a new shape.
class ModelKernelLayout {
public:
    static std::optional<ModelKernelLayout> bind_page_table(std::size_t source_call, const SourceKernel & kernel, std::span<const SourceRequest> requests,
                                                            std::uint32_t page_size_tokens);
    static std::optional<ModelKernelLayout> bind_forward_inputs(std::size_t source_call, const SourceKernel & kernel, std::int64_t tokens,
                                                                std::int64_t requests, StepKind step, bool foreach_copy);
    static std::optional<ModelKernelLayout> bind(const SourceNativeRegion & region, std::size_t source_call, const SourceKernel & kernel,
                                                 std::int64_t serving_tokens, std::int64_t requests, StepKind step, const SourceNativeRegion * parameter_child);
    static std::optional<ModelKernelLayout> bind_sequence_lengths(const SourceNativeRegion & region, std::size_t source_call, const SourceKernel & kernel,
                                                                  std::span<const SourceRequest> requests);
    static std::optional<ModelKernelLayout> bind_sampling_positions(const SourceNativeRegion & region, std::size_t source_call, const SourceKernel & kernel,
                                                                    std::span<const SourceRequest> requests, StepKind step);
    ModelParameterRule compile(const SourceKernel & source) const;
    std::optional<ModelKernelLayout> bind_sampling(std::size_t source_call, const SourceKernel & kernel) const;
    ModelKernelRole role() const { return role_; }
    bool has_partial_operands() const { return role_ == ModelKernelRole::FusedQkv; }
    nlohmann::json describe() const;

private:
    static std::optional<ModelKernelLayout> bind_positions(const SourceNativeRegion & region, std::size_t source_call, const SourceKernel & kernel,
                                                           std::int64_t tokens);
    static std::optional<ModelKernelLayout> bind_logits_output(const SourceNativeRegion & region, const SourceNativeRegion * selection, std::size_t source_call,
                                                               const SourceKernel & kernel, std::int64_t tokens, std::int64_t requests, StepKind step);
    static std::optional<ModelKernelLayout> bind_logits_selection(const SourceNativeRegion & region, std::size_t source_call, const SourceKernel & kernel,
                                                                  std::int64_t tokens, std::int64_t requests, StepKind step);
    static std::optional<ModelKernelLayout> bind_embedding(const SourceNativeRegion & region, std::size_t source_call, const SourceKernel & kernel,
                                                           std::int64_t tokens);
    static std::optional<ModelKernelLayout> bind_attention_rows(const SourceNativeRegion & region, std::size_t source_call, const SourceKernel & kernel,
                                                                std::int64_t tokens);
    static std::optional<ModelKernelLayout> bind_norm(const SourceNativeRegion & region, std::size_t source_call, const SourceKernel & kernel,
                                                      std::int64_t serving_tokens, const SourceNativeRegion * parameter_child);
    static bool matches(const KernelShapes & inputs, const KernelShapes & outputs, std::int64_t tokens, std::int64_t input_width, std::int64_t output_width,
                        bool linear);
    ModelKernelLayout(ModelKernelRole role, std::size_t source_call, std::int64_t tokens, std::int64_t input_width, std::int64_t output_width);

    ModelKernelRole role_;
    std::size_t source_call_;
    std::int64_t source_tokens_;
    std::int64_t input_width_;
    std::int64_t output_width_;
    using AxisExtent = TensorAxisExtent;
    std::int64_t page_size_tokens_ = 0;
    std::int64_t source_requests_ = 0;
    using TokenAxis = TensorParameterAxis;
    std::vector<TokenAxis> input_token_axes_{
        { 0, 1 }
    };
    std::vector<TokenAxis> output_token_axes_{
        { 0, 1 }
    };
};

} // namespace markov
