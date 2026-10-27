#include "markov/trace_graph/structure/phase_shape.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/structure/tensor_program.hpp"

#include <type_traits>

namespace markov::trace_graph::structure {
namespace {

// Ordinary single-request metadata first selects token indices up to the
// current context, then samples every page_size-th index to build a page
// table. These are active work counts, not graph-buffer capacity or costs.
void append_page_table_work(std::map<std::string, double> & features, uint64_t context_tokens, uint64_t batch_size, uint64_t page_size_tokens) {
    const auto columns = context_tokens / page_size_tokens + static_cast<uint64_t>(context_tokens % page_size_tokens != 0);
    features.emplace("page_table_columns", static_cast<double>(columns));
    features.emplace("page_table_active_entries", static_cast<double>(batch_size) * static_cast<double>(columns));
    features.emplace("page_table_index_entries", static_cast<double>(batch_size) * static_cast<double>(context_tokens));
}

} // namespace

std::string_view tensor_execution_name(TensorExecutionKind kind) {
    switch (kind) {
    case TensorExecutionKind::AsyncDevice:
        return "async_device";
    case TensorExecutionKind::CpuService:
        return "cpu_service";
    case TensorExecutionKind::BlockingContextReadback:
        return "blocking_readback";
    case TensorExecutionKind::DirectBlockingDevice:
        return "direct_blocking_device";
    case TensorExecutionKind::LayerReady:
        return "hicache_layer_ready";
    }
    return "unknown";
}

double PrefillShape::attention_work_pairs() const {
    return static_cast<double>(new_tokens) * (static_cast<double>(context_tokens) + static_cast<double>(new_tokens) / 2.0);
}

std::map<std::string, double> CollectiveShape::features() const {
    const auto elements = core::checked_multiply_u64(rows, width, "collective element count overflow");
    const auto bytes = core::checked_multiply_u64(elements, element_bytes, "collective byte count overflow");
    return {
        {                 "rows",          static_cast<double>(rows) },
        {                "width",         static_cast<double>(width) },
        {        "element_bytes", static_cast<double>(element_bytes) },
        { "logical_tensor_bytes",         static_cast<double>(bytes) },
        {         "participants",  static_cast<double>(participants) }
    };
}

std::map<std::string, double> LinearShape::features() const {
    return {
        {                         "m",                                                   static_cast<double>(m) },
        {                         "n",                                                   static_cast<double>(n) },
        {                         "k",                                                   static_cast<double>(k) },
        { "multiply_accumulate_count", static_cast<double>(m) * static_cast<double>(n) * static_cast<double>(k) },
        {            "input_elements",                          static_cast<double>(m) * static_cast<double>(k) },
        {           "weight_elements",                          static_cast<double>(n) * static_cast<double>(k) },
        {           "output_elements",                          static_cast<double>(m) * static_cast<double>(n) },
        {            "input_stride_0",                                    static_cast<double>(input_row_stride) },
        {            "input_stride_1",                                 static_cast<double>(input_column_stride) },
        {           "weight_stride_0",                                   static_cast<double>(weight_row_stride) },
        {           "weight_stride_1",                                static_cast<double>(weight_column_stride) }
    };
}

std::optional<LinearShape> packed_linear_shape(const LinearShape & source, uint64_t target_rows) {
    if (source.input_column_stride != 1 || source.input_row_stride != source.k) return std::nullopt;

    auto target = source;
    target.m = target_rows;
    return target;
}

std::map<std::string, double> GatedActivationShape::features() const {
    return {
        {            "rows",                              static_cast<double>(rows) },
        {           "width",                             static_cast<double>(width) },
        { "output_elements", static_cast<double>(rows) * static_cast<double>(width) },
        {  "input_stride_0",                  static_cast<double>(input_row_stride) },
        {  "input_stride_1",               static_cast<double>(input_column_stride) }
    };
}

uint64_t dense_rows(const DenseShape & shape) {
    return std::visit(
        [](const auto & value) {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, LinearShape>) return value.m;
            else return value.rows;
        },
        shape);
}

std::map<std::string, double> EmbeddingShape::features() const {
    return {
        {            "rows",                              static_cast<double>(rows) },
        { "vocabulary_rows",                   static_cast<double>(vocabulary_rows) },
        {           "width",                             static_cast<double>(width) },
        { "output_elements", static_cast<double>(rows) * static_cast<double>(width) }
    };
}

std::map<std::string, double> EmbeddingMaskShape::features() const {
    return {
        {     "rows", static_cast<double>(rows) },
        { "elements", static_cast<double>(rows) }
    };
}

std::map<std::string, double> EmbeddingOutputMaskShape::features() const {
    return {
        {     "rows",                              static_cast<double>(rows) },
        {    "width",                             static_cast<double>(width) },
        { "elements", static_cast<double>(rows) * static_cast<double>(width) }
    };
}

std::map<std::string, double> NormalizationShape::features() const {
    return {
        {           "rows",                              static_cast<double>(rows) },
        {          "width",                             static_cast<double>(width) },
        {       "elements", static_cast<double>(rows) * static_cast<double>(width) },
        {        "epsilon",                                                epsilon },
        { "input_stride_0",                  static_cast<double>(input_row_stride) },
        { "input_stride_1",               static_cast<double>(input_column_stride) }
    };
}

std::map<std::string, double> LogitsOutputShape::features() const {
    return {
        {             "rows",                                         static_cast<double>(rows) },
        {      "input_width",                                  static_cast<double>(input_width) },
        { "vocabulary_width",                             static_cast<double>(vocabulary_width) },
        {  "output_elements", static_cast<double>(rows) * static_cast<double>(vocabulary_width) }
    };
}

std::map<std::string, double> LogitsPruningShape::features() const {
    return {
        {    "input_rows",  static_cast<double>(rows) },
        {         "width", static_cast<double>(width) },
        { "selected_rows",                        1.0 }
    };
}

std::map<std::string, double> InputBuffersShape::features() const {
    return {
        {        "input_rows",            static_cast<double>(rows) },
        { "device_copy_count",   static_cast<double>(copies.size()) },
        {   "host_copy_count", static_cast<double>(host_copy_count) }
    };
}

std::map<std::string, double> GreedySamplingShape::features() const {
    return {
        {             "rows",                                         static_cast<double>(rows) },
        { "vocabulary_width",                             static_cast<double>(vocabulary_width) },
        {         "elements", static_cast<double>(rows) * static_cast<double>(vocabulary_width) }
    };
}

std::optional<DenseShape> target_dense_shape(const DenseShape & source, uint64_t target_rows) {
    return std::visit(
        [&](const auto & value) -> std::optional<DenseShape> {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, LinearShape>) {
                const auto target = packed_linear_shape(value, target_rows);
                if (!target) return std::nullopt;
                return *target;
            }
            else {
                if constexpr (std::is_same_v<std::decay_t<decltype(value)>, GatedActivationShape>) {
                    if (value.input_column_stride != 1 || value.input_row_stride / 2 != value.width || value.input_row_stride % 2 != 0) return std::nullopt;
                }
                else if constexpr (std::is_same_v<std::decay_t<decltype(value)>, NormalizationShape>) {
                    // A single-head Q/K slice retains the packed QKV row pitch;
                    // changing token count does not change that pitch or width.
                    if (value.input_column_stride != 1 || value.input_row_stride < value.width) return std::nullopt;
                }

                auto target = value;
                target.rows = target_rows;
                if constexpr (std::is_same_v<std::decay_t<decltype(value)>, RotaryShape>) {
                    if (selects_fused_rotary(target)) return std::nullopt;
                }
                return target;
            }
        },
        source);
}

std::map<std::string, double> QkvRopeShape::features() const {
    return {
        {                      "rows",        static_cast<double>(rows) },
        {               "query_width", static_cast<double>(query_width) },
        {                  "kv_width",    static_cast<double>(kv_width) },
        {                  "head_dim",    static_cast<double>(head_dim) },
        {                  "rope_dim",    static_cast<double>(rope_dim) },
        {                   "epsilon",                          epsilon },
        { "triton_task_queue_enabled",           task_queue ? 1.0 : 0.0 },
        {                "neox_style",           neox_style ? 1.0 : 0.0 }
    };
}

std::map<std::string, double> RotaryTableShape::features() const {
    return {
        {       "rows",                              static_cast<double>(rows) },
        {      "width",                             static_cast<double>(width) },
        { "table_rows",                        static_cast<double>(table_rows) },
        {   "elements", static_cast<double>(rows) * static_cast<double>(width) }
    };
}

std::map<std::string, double> RotaryShape::features() const {
    return {
        {        "rows",        static_cast<double>(rows) },
        { "query_width", static_cast<double>(query_width) },
        {    "kv_width",    static_cast<double>(kv_width) },
        {    "head_dim",    static_cast<double>(head_dim) },
        {  "table_rows",  static_cast<double>(table_rows) },
        {  "neox_style",           neox_style ? 1.0 : 0.0 }
    };
}

bool selects_fused_rotary(const RotaryShape & shape) {
    constexpr uint64_t fused_dispatch_element_limit = 65'535;
    return shape.native_float_table && shape.fused_rope_available
           && core::checked_multiply_u64(shape.rows, shape.query_width, "Rotary dispatch size overflow") < fused_dispatch_element_limit;
}

bool QkReshapeShape::copies_input() const {
    return rows > 1 && heads > 1 && input_row_stride != core::checked_multiply_u64(heads, head_dim, "Q/K reshape width overflow");
}

std::map<std::string, double> QkReshapeShape::features() const {
    return {
        {             "rows",                                                              static_cast<double>(rows) },
        {            "heads",                                                             static_cast<double>(heads) },
        {         "head_dim",                                                          static_cast<double>(head_dim) },
        { "input_row_stride",                                                  static_cast<double>(input_row_stride) },
        {         "elements", static_cast<double>(rows) * static_cast<double>(heads) * static_cast<double>(head_dim) }
    };
}

std::map<std::string, double> PrefillShape::features() const {
    std::map<std::string, double> result{
        {            "new_tokens",       static_cast<double>(new_tokens) },
        {        "context_tokens",   static_cast<double>(context_tokens) },
        { "attention_token_pairs",                attention_work_pairs() },
        {            "batch_size",       static_cast<double>(batch_size) },
        {      "page_size_tokens", static_cast<double>(page_size_tokens) },
    };
    const auto full_context = core::checked_add_u64(context_tokens, new_tokens, "Prefill context overflow");
    append_page_table_work(result, full_context, batch_size, page_size_tokens);
    return result;
}

std::map<std::string, double> DecodeShape::features() const {
    const auto pages = context_tokens / page_size_tokens + static_cast<uint64_t>(context_tokens % page_size_tokens != 0);
    std::map<std::string, double> result{
        {   "context_tokens",   static_cast<double>(context_tokens) },
        { "page_size_tokens", static_cast<double>(page_size_tokens) },
        {      "cache_pages",            static_cast<double>(pages) },
        {       "batch_size",       static_cast<double>(batch_size) },
        { "decode_iteration",        static_cast<double>(iteration) },
    };
    append_page_table_work(result, context_tokens, batch_size, page_size_tokens);
    return result;
}

uint64_t decode_context_tokens(uint64_t prompt_tokens, uint64_t iteration) {
    return core::checked_add_u64(prompt_tokens, core::checked_add_u64(iteration, 1, "Decode iteration overflow"), "Decode context overflow");
}

} // namespace markov::trace_graph::structure
