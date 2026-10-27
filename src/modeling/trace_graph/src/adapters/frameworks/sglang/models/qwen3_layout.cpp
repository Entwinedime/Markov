#include "markov/trace_graph/adapters/frameworks/sglang/source_calls.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/source_dag/index.hpp"

#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace markov::trace_graph::adapters::sglang {
namespace {

using Identity = std::tuple<size_t, std::optional<uint64_t>, uint64_t>;
using Producers = std::map<std::string, std::vector<const core::TraceEvent *>>;

std::optional<uint64_t> packed_row_stride(const nlohmann::json & fields, const std::string & name, uint64_t rows, uint64_t elements) {
    const auto shape = fields.find(name + "_shape");
    const auto stride = fields.find(name + "_stride");
    if (shape == fields.end() || stride == fields.end() || !shape->is_array() || !stride->is_array() || shape->size() < 2 || stride->size() != shape->size()
        || rows == 0 || elements % rows != 0 || shape->at(0) != rows)
        return std::nullopt;

    uint64_t width = 1;
    for (size_t dimension = shape->size() - 1; dimension > 0; --dimension) {
        const auto size = shape->at(dimension).get<int64_t>();
        if (size <= 0 || stride->at(dimension) != width || static_cast<uint64_t>(size) > elements / width) return std::nullopt;
        width *= static_cast<uint64_t>(size);
    }
    const auto row_stride = stride->at(0).get<int64_t>();
    if (width != elements / rows || row_stride < 0 || static_cast<uint64_t>(row_stride) < width) return std::nullopt;
    return static_cast<uint64_t>(row_stride);
}

const core::TraceEvent * unique_producer(const Producers & producers, const std::string & role, const core::TraceEvent & consumer) {
    const auto found = producers.find(role);
    if (found == producers.end() || found->second.size() != 1) return nullptr;
    const auto * event = found->second.front();
    if (event->pid != consumer.pid || event->tid != consumer.tid || event->ts > consumer.ts || event->dur > consumer.ts - event->ts) return nullptr;
    return event;
}

} // namespace

void resolve_attention_cache_layouts(const source_dag::SourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                     const PhaseCallPositions & positions, AttentionCallAudit & attention) {
    const auto & graph = source.graph();
    const auto & events = graph.runtime_observations();
    std::vector<std::pair<size_t, uint64_t>> unresolved;
    for (size_t index = 0; index < attention.calls.size(); ++index) {
        auto & call = attention.calls[index];
        auto & write = call.cache_write;
        if (!write.enabled) continue;

        const auto & position = *call.region.descriptor;
        const auto & descriptor = position.prelude ? graph.prelude_context_events().at(position.observation) : events.at(position.observation);
        const auto fields = nlohmann::json::parse(descriptor.args_json_view());
        const auto & cache = fields.at("cache_write");
        if (cache.contains("key_shape") || cache.contains("value_shape")) {
            write.key_row_stride = packed_row_stride(cache, "key", write.locations, write.key_elements);
            write.value_row_stride = packed_row_stride(cache, "value", write.locations, write.value_elements);
        }
        else unresolved.emplace_back(index, fields.at("layer_id").get<uint64_t>());
    }
    if (unresolved.empty()) return;

    std::map<Identity, Producers> producers;
    for (size_t index = 0; index < events.size(); ++index) {
        const auto & event = events[index];
        const bool projection = event.name == "runtime.phase.linear" && event.arg("dense_role") == "qkv_projection";
        if (!projection && event.name != "runtime.phase.rotary" && event.name != "runtime.phase.qkv_norm_rope") continue;
        if (event.arg("status") != "returned" || event.arg("model_layer_implementation") != "sglang.srt.models.qwen3.Qwen3DecoderLayer"
            || !event.has_arg("model_layer_index"))
            continue;

        const auto region = source_dag::observe_submitted_call_region(source, queues, index);
        const auto owner = call_phase_position(region, positions);
        if (!owner) continue;
        producers[{ owner->phase_index, owner->decode_iteration, event.arg_u64("model_layer_index") }][event.name].push_back(&event);
    }

    for (const auto & [index, layer] : unresolved) {
        auto & call = attention.calls[index];
        auto & write = call.cache_write;
        // Older descriptors can still be explained by recorded producers,
        // joined by actual phase ownership and layer, not nearest timestamps.
        const auto found = producers.find({ call.position.phase_index, call.position.decode_iteration, layer });
        if (found == producers.end()) continue;
        const auto & consumer = events.at(call.region.work.observation);
        if (call.position.decode_iteration) {
            const auto * fused = unique_producer(found->second, "runtime.phase.qkv_norm_rope", consumer);
            if (!fused || fused->arg("callsite") != "forward_prepare_npu" || fused->arg("key_dtype") != write.key_dtype
                || fused->arg("value_dtype") != write.value_dtype)
                continue;
            const auto outputs = nlohmann::json::parse(fused->args_json_view());
            write.key_row_stride = packed_row_stride(outputs, "key", write.locations, write.key_elements);
            write.value_row_stride = packed_row_stride(outputs, "value", write.locations, write.value_elements);
            continue;
        }

        const auto * projection = unique_producer(found->second, "runtime.phase.linear", consumer);
        const auto * rotary = unique_producer(found->second, "runtime.phase.rotary", consumer);
        if (!projection || !rotary || rotary->arg("callsite") != "forward_prepare_native" || projection->arg("output_dtype") != write.value_dtype
            || rotary->arg("key_dtype") != write.key_dtype)
            continue;

        const auto outputs = nlohmann::json::parse(projection->args_json_view());
        const auto query_width = core::checked_multiply_u64(call.batch.query_heads, call.conditions.query_key_dim, "QKV query width overflow");
        const auto query_elements = core::checked_multiply_u64(query_width, write.locations, "QKV query size overflow");
        const auto kv_elements = core::checked_add_u64(write.key_elements, write.value_elements, "QKV cache size overflow");
        const auto packed_elements = core::checked_add_u64(query_elements, kv_elements, "QKV projection size overflow");
        const auto projection_stride = packed_row_stride(outputs, "output", write.locations, packed_elements);
        if (!projection_stride || *projection_stride != packed_elements / write.locations) continue;
        // Qwen3 native preparation splits packed Q/K/V; normalization and
        // RoPE replace K, while V remains the original projection's view.
        write.key_row_stride = packed_row_stride(nlohmann::json::parse(rotary->args_json_view()), "key", write.locations, write.key_elements);
        write.value_row_stride = projection_stride;
    }
}

} // namespace markov::trace_graph::adapters::sglang
