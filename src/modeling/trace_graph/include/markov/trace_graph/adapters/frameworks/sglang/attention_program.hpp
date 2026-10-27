#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "markov/trace_graph/structure/operation.hpp"
#include "markov/trace_graph/structure/tensor_program.hpp"

namespace markov::trace_graph::adapters::sglang {

/** Existing native family classification, not proof of an exact operator ABI. */
[[nodiscard]] bool is_attention_kernel(std::string_view name);

/** Execution conditions, not observed kernels or fitted coefficients.
 * Unknown required conditions must be resolved at the source/config boundary.
 */
struct AscendAttentionConditions {
    bool prefill;
    bool dllm, sparse, verify, draft;
    bool mla, graph, compile;
    bool sinks, hybrid_swa, swa_layer;
    bool context_parallel;
    bool fia, causal, encoder, native_sdpa;
    std::optional<bool> alibi;
    double logit_cap;
    uint64_t query_key_dim, value_dim;
    std::optional<bool> sequence_lengths_on_cpu;
    std::optional<bool> layer_counter_registered;
    std::optional<uint64_t> cache_layer_index;
    std::string cache_pool_implementation;
    std::optional<bool> torch_ops_fia_batch_invariant;
};

/** Framework dispatch only. Selecting a route does not yet expand its tensor
 * program, establish native kernel counts, or assign resource dependencies.
 */
struct AttentionDispatch {
    std::string branch;
    std::string layout;
    bool per_request = false;
};

[[nodiscard]] std::optional<AttentionDispatch> ascend_attention_dispatch(const AscendAttentionConditions & input);

struct AttentionRequestShape {
    uint64_t query_tokens;
    uint64_t context_tokens;
};

/** Target batch geometry, validated by the framework input boundary.
 * context_tokens includes the new query tokens; padded_query_tokens is the
 * tensor's leading dimension, not the number of real tokens.
 */
struct AttentionBatchShape {
    std::vector<AttentionRequestShape> requests;
    uint64_t padded_query_tokens;
    uint64_t query_heads, kv_heads, page_tokens;
};

/** Ordinary one-request int32 page table from a contiguous token-index
 * gather, sliced at page_tokens stride, divided by an integer scalar.
 * Describes that tensor call only, not the surrounding metadata program.
 * Input domain: positive context/page sizes; current Ascend910B lowering. */
[[nodiscard]] std::optional<structure::TensorOperation> ascend_page_table_division(const std::string & component, uint64_t context_tokens, uint64_t page_tokens,
                                                                                   std::string_view device_name);

/** Ordinary non-speculative single-request metadata preparation. Sequence
 * and index tensors reside on device; the request table is contiguous int32.
 * Sizes are positive and context_tokens <= max_context_tokens. Graph refers
 * to replay preparation, not graph capture. Optional extend dtype denotes
 * an actual device-to-host transfer; CPU-only extension handling stays in
 * preparation service. Dtypes are validated int32/int64 at the input boundary. */
struct AttentionMetadataInputs {
    bool graph;
    uint64_t context_tokens, page_tokens, max_context_tokens;
    std::string device_name, sequence_dtype;
    std::optional<std::string> extend_readback_dtype;
    // Graph replay may pass the captured sequence buffer back to itself.
    // Exact aliasing eliminates device copy, not its caller-side work.
    bool sequence_copy_identity = false;
    uint64_t request_table_rows, request_table_columns;
};

/** Framework-level operations and CPU boundaries, independent of cost.
 * Includes page-table gather/division, eager max/readback and conversions,
 * or graph buffer copy/clear. Indexing and division have explicit native lowering;
 * other tensor slots are not proof of a single native kernel. */
[[nodiscard]] std::optional<std::vector<structure::TensorOperation>> ascend_attention_metadata_program(const std::string & component,
                                                                                                       const AttentionMetadataInputs & input);

/** Ordinary MHA writes without CP gathering or quantization scales. The cache
 * pool's FIA switch is independent of the selected attention dispatch.
 */
struct AttentionCacheWrite {
    bool enabled;
    bool fia;
    std::string cache_dtype, key_dtype, value_dtype, location_dtype;
    uint64_t key_elements, value_elements, locations;
    // Element strides between token rows; inner head dimensions are packed.
    // Missing layout is not evidence of a contiguous tensor.
    std::optional<uint64_t> key_row_stride, value_row_stride;
};

struct AttentionEagerPadding {
    bool mlp_sync, allow_input_scattered;
    uint64_t tp_size;
    // A property of the target batch, not inherited from a source execution.
    bool can_run_tbo;
};

struct AttentionInputs {
    AttentionBatchShape batch;
    AttentionCacheWrite cache_write;
};

/** Generate ordinary eager MHA inputs from validated target request lengths.
 * Base supplies deployment metadata and verifies the ordinary packed KV
 * layout, never target token counts or a padding ratio. Graph and distributed
 * MLP-sync padding need their own rules and return unavailable here.
 */
[[nodiscard]] std::optional<AttentionInputs> target_eager_attention_inputs(const AscendAttentionConditions & conditions,
                                                                           const AttentionBatchShape & source_batch, const AttentionCacheWrite & source_write,
                                                                           const AttentionEagerPadding & padding,
                                                                           std::span<const AttentionRequestShape> target_requests, uint64_t target_page_tokens);

/** Expand ordinary MHA cache-write and FIA/non-FIA score/output tensor programs, independently of source
 * kernel counts and costs. Null means an unimplemented route, not empty work.
 * Views/empty allocation have no device operation here. CPU preparation is
 * placed immediately before each tensor call or layer check; the first such
 * interval is the outer entry. The final return is separate. CPU work inside
 * a tensor-call envelope is not included again in these outer intervals. Cache-write completion
 * precedes attention score work.
 * Graph decode includes captured score work, not a workspace query per replay.
 * Tensor operations still require backend lowering before native DAG mutation.
 */
[[nodiscard]] std::optional<std::vector<structure::TensorOperation>>
    ascend_attention_tensor_program(const AscendAttentionConditions & conditions, const AttentionBatchShape & batch, const AttentionCacheWrite & write);

} // namespace markov::trace_graph::adapters::sglang
