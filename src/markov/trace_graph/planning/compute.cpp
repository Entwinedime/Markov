#include "markov/trace_graph/planning/compute.hpp"

#include <utility>

namespace markov::trace_graph::planning {

std::vector<Work> ComputeOperation::describe(Phase phase, const ComputeInput & input) const {
    const std::string prefix = phase == Phase::Prefill ? "prefill." : "decode.";
    if (const auto * shape = std::get_if<LinearGeometry>(&geometry)) {
        const auto m = std::get<RowInput>(input).rows;
        return {
            Work{ { prefix + "linear", implementation, "device_service" },
                 { { "m", m },
                    { "n", shape->n },
                    { "k", shape->k },
                    { "multiply_accumulate_count", m * shape->n * shape->k },
                    { "input_elements", m * shape->k },
                    { "output_elements", m * shape->n },
                    { "weight_elements", shape->n * shape->k },
                    { "input_stride_0", shape->input_stride[0] },
                    { "input_stride_1", shape->input_stride[1] },
                    { "weight_stride_0", shape->weight_stride[0] },
                    { "weight_stride_1", shape->weight_stride[1] } } }
        };
    }
    if (const auto * shape = std::get_if<NormalizationGeometry>(&geometry)) {
        const auto rows = std::get<RowInput>(input).rows;
        return {
            Work{ { prefix + "rms_norm", implementation, "device_service" },
                 { { "rows", rows },
                    { "width", shape->width },
                    { "elements", rows * shape->width },
                    { "epsilon", shape->epsilon },
                    { "input_stride_0", shape->input_stride[0] },
                    { "input_stride_1", shape->input_stride[1] } } }
        };
    }
    if (const auto * shape = std::get_if<ActivationGeometry>(&geometry)) {
        const auto rows = std::get<RowInput>(input).rows;
        return {
            Work{ { prefix + "gated_activation", implementation, "device_service" },
                 { { "rows", rows },
                    { "width", shape->width },
                    { "output_elements", rows * shape->width },
                    { "input_stride_0", shape->input_stride[0] },
                    { "input_stride_1", shape->input_stride[1] } } }
        };
    }

    if (const auto * shape = std::get_if<AdditionGeometry>(&geometry)) {
        const auto rows = std::get<RowInput>(input).rows;
        return {
            Work{ { prefix + "split_add_rms_norm.add", implementation, "device_service" },
                 { { "rows", rows },
                    { "width", shape->width },
                    { "elements", rows * shape->width },
                    { "input_stride_0", shape->input_stride[0] },
                    { "input_stride_1", shape->input_stride[1] } } }
        };
    }

    if (const auto * shape = std::get_if<CacheWriteGeometry>(&geometry)) {
        const auto rows = std::get<RowInput>(input).rows;
        Features features{
            { "locations",                rows },
            {  "elements", rows * shape->width }
        };
        const auto operation = prefix + shape->operation;

        // A single row is contiguous even when its unused outer stride differs.
        // Materialization is real work, not a multiplier on the scatter cost.
        if (rows > 1 && shape->source_row_stride != shape->width) {
            features.emplace("source_row_stride", shape->source_row_stride);
            return {
                {    { operation + ".materialize", shape->materialize_implementation, "device_service" },            features },
                { { operation + ".scatter", shape->materialized_write_implementation, "device_service" }, std::move(features) }
            };
        }
        return {
            Work{ { operation, implementation, "device_service" }, std::move(features) }
        };
    }

    const auto & shape = std::get<AttentionGeometry>(geometry);
    const auto & attention = std::get<AttentionInput>(input);
    const auto q = attention.query_tokens;

    // Causal queries attend the retained prefix plus their own growing suffix.
    const auto pairs = shape.causal ? q * (attention.context_tokens - q) + q * (q + 1) / 2 : q * attention.context_tokens;
    return {
        Work{ { prefix + "attention.score", implementation, "device_service" },
             { { "query_tokens", q },
                { "tensor_query_tokens", attention.tensor_query_tokens },
                { "context_tokens", attention.context_tokens },
                { "attention_pairs", pairs },
                { "batch_size", attention.batch_size },
                { "page_tokens", attention.page_tokens },
                { "query_heads", shape.query_heads },
                { "kv_heads", shape.kv_heads },
                { "query_key_dim", shape.query_key_dim },
                { "value_dim", shape.value_dim } } }
    };
}

} // namespace markov::trace_graph::planning
