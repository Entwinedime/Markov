#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace markov::trace_graph::structure {

/** A two-dimensional x[M,K] times weight[N,K] linear operation.
 * Strides are in elements. Token ownership is deliberately not encoded by
 * equal dimension values; a framework adapter must establish that relation. */
struct LinearShape {
    uint64_t m, n, k;
    uint64_t input_row_stride, input_column_stride, weight_row_stride, weight_column_stride;

    /** MAC count is M*N*K (not twice that value); element counts describe
     * logical tensors, not measured DRAM traffic or padded hardware work. */
    [[nodiscard]] std::map<std::string, double> features() const;
};

/** Packed token-major input only, after the framework establishes the target
 * number of rows. Weights/layout stay fixed; no fitted ratio or kernel-name
 * inference. Other input layouts require an explicit target layout rule. */
[[nodiscard]] std::optional<LinearShape> packed_linear_shape(const LinearShape & source, uint64_t target_rows);

/** Fused SiLU(gate) * value: input [rows, 2*width], output [rows, width].
 * This is elementwise work, not a matrix product with invented weights. */
struct GatedActivationShape {
    uint64_t rows, width;
    uint64_t input_row_stride, input_column_stride;

    [[nodiscard]] std::map<std::string, double> features() const;
};

/** One-dimensional int64 token indices gathering rows of a packed local
 * vocabulary shard. Logical output elements do not imply DRAM transactions
 * or known index locality; TP masking and reduction are separate work. */
struct EmbeddingShape {
    uint64_t rows, vocabulary_rows, width;

    [[nodiscard]] std::map<std::string, double> features() const;
};

/** Eager int64 vocabulary-shard index preparation, before embedding lookup.
 * Work is elementwise; token values and mask density are not observed. */
struct EmbeddingMaskShape {
    uint64_t rows;

    [[nodiscard]] std::map<std::string, double> features() const;
};

/** Broadcast a bool mask [rows,1] over the local embedding [rows,width],
 * writing zero in place. Independent from lookup and all-reduce. */
struct EmbeddingOutputMaskShape {
    uint64_t rows, width;

    [[nodiscard]] std::map<std::string, double> features() const;
};

/** Final vocabulary slice and float32 output. A buffer copy or dtype
 * conversion does device work; an already-float32 slice is only a view. */
struct LogitsOutputShape {
    uint64_t rows, input_width, vocabulary_width;
    bool copies_input;

    [[nodiscard]] std::map<std::string, double> features() const;
};

/** Ordinary single-request Prefill selects the last hidden-state row before
 * the vocabulary projection. Length cumsum/subtract operates on one int32
 * length, promoted to int64; row selection reads from [rows,width]. No input
 * logprobs, static padding, auxiliary states or Decode passthrough here. */
struct LogitsPruningShape {
    uint64_t rows, width;

    [[nodiscard]] std::map<std::string, double> features() const;
};

/** One ordinary eager buffer copy. Axis is declared by the framework slot,
 * never inferred from a dimension equal to the current token count. */
struct InputBufferCopy {
    std::string role, source_dtype, destination_dtype;
    bool token_axis;
};

/** Single-request eager buffers, no padding or custom fill callbacks. */
struct InputBuffersShape {
    uint64_t rows;
    std::vector<InputBufferCopy> copies;
    uint64_t host_copy_count = 0;

    [[nodiscard]] std::map<std::string, double> features() const;
};

/** Greedy argmax over the vocabulary for each request's last-token logits.
 * Does not describe logprob calculation or stochastic sampling. */
struct GreedySamplingShape {
    uint64_t rows, vocabulary_width;

    [[nodiscard]] std::map<std::string, double> features() const;
};

/** Per-row RMS reduction with optional residual addition, fused or explicit
 * in batch-invariant execution. Both inputs use the same validated layout;
 * the explicit addition returns packed storage. Weights are a packed vector. */
struct NormalizationShape {
    uint64_t rows, width;
    uint64_t input_row_stride, input_column_stride;
    double epsilon;
    bool residual;
    bool split_residual = false;

    [[nodiscard]] std::map<std::string, double> features() const;
};

/** Qwen3 NPU fused split/RMSNorm/RoPE, packed buffers and no bias. */
struct QkvRopeShape {
    uint64_t rows, query_width, kv_width, head_dim, rope_dim;
    double epsilon;
    bool task_queue, neox_style;

    [[nodiscard]] std::map<std::string, double> features() const;
};

struct RotaryTableShape {
    uint64_t rows, width, table_rows;

    [[nodiscard]] std::map<std::string, double> features() const;
};

/** Ordinary full-head npu_mrope with packed 2-D Q/K and 1-D positions. */
struct RotaryShape {
    uint64_t rows, query_width, kv_width, head_dim, table_rows;
    bool neox_style;
    // Float-table/BF16 fallback without a cached position table. The optional
    // fused implementation affects target dispatch even when the source took
    // the eager elementwise branch, which is not npu_mrope.
    bool native_float_table = false;
    bool fused_rope_available = false;

    [[nodiscard]] std::map<std::string, double> features() const;
};

/** SGLang's optional RoPE dispatch uses rows * query width for packed 2-D
 * inputs. This is a framework branch condition, not a fitted cost threshold.
 * Selecting it does not establish that the installed kernel accepts 2-D. */
[[nodiscard]] bool selects_fused_rotary(const RotaryShape & shape);

struct QkReshapeShape {
    uint64_t rows, heads, head_dim, input_row_stride;

    [[nodiscard]] bool copies_input() const;
    [[nodiscard]] std::map<std::string, double> features() const;
};

/** Logical all-reduce payload, not network wire traffic or algorithm bytes.
 * Arrival spread is execution state and is added when service becomes ready. */
struct CollectiveShape {
    uint64_t rows, width, element_bytes, participants;

    [[nodiscard]] std::map<std::string, double> features() const;
};

using DenseShape =
    std::variant<LinearShape, GatedActivationShape, EmbeddingShape, EmbeddingMaskShape, EmbeddingOutputMaskShape, LogitsOutputShape, LogitsPruningShape,
                 InputBuffersShape, GreedySamplingShape, NormalizationShape, QkvRopeShape, RotaryTableShape, RotaryShape, QkReshapeShape>;

[[nodiscard]] uint64_t dense_rows(const DenseShape & shape);
[[nodiscard]] std::optional<DenseShape> target_dense_shape(const DenseShape & source, uint64_t target_rows);

/** Work geometry shared by observation export and target cost requests.
 * These are already validated single-request, non-speculative shapes, not
 * model parameters or a description of the backend's kernel topology.
 * Page-table features describe the active index rectangle. They do not
 * include graph replay's unused-buffer clearing or authorize kernel lowering.
 */
struct PrefillShape {
    uint64_t new_tokens, context_tokens, batch_size, page_size_tokens;

    /** Quadratic work proxy n * (context + n/2), not an exact causal-pair
     * count. Keep its definition identical when training and predicting.
     */
    [[nodiscard]] double attention_work_pairs() const;
    [[nodiscard]] std::map<std::string, double> features() const;
};

struct DecodeShape {
    uint64_t context_tokens, batch_size, page_size_tokens, iteration;

    [[nodiscard]] std::map<std::string, double> features() const;
};

/** Prefill produced the first output token. The zero-based decode round
 * allocates its next token and increments seq_lens before attention runs.
 */
[[nodiscard]] uint64_t decode_context_tokens(uint64_t prompt_tokens, uint64_t iteration);

} // namespace markov::trace_graph::structure
