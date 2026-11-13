#include "markov/adapters/prediction.hpp"
#include "markov/trace_graph/planning/cache_program.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

namespace markov::adapters {
namespace {

using Json = nlohmann::json;

std::size_t positive(const Json & raw, const char * name) {
    const auto value = raw.at(name).get<std::int64_t>();
    if (!raw.at(name).is_number_integer() || value <= 0) throw std::runtime_error(std::string(name) + " must be a positive integer");
    return value;
}

} // namespace

PreparedPrediction prepare_prediction(const std::filesystem::path & path) {
    std::ifstream input(path);
    const auto raw = Json::parse(input);
    auto scaling = read_node_scale(raw.value("node_scale", Json::array()));
    const auto manifest = trace_graph::source::inspect_manifest(raw.at("manifest").get<std::string>());
    if (manifest.framework != "sglang") throw std::runtime_error("No prediction adapter for " + manifest.framework);
    const auto deployment = raw.at("deployment");
    std::map<std::size_t, SelectedCosts> costs;
    std::map<std::size_t, std::filesystem::path> models_paths;
    for (const auto & [tp, file] : raw.at("models").items()) {
        const auto participants = std::stoul(tp);
        auto selected = read_cost_models(file.get<std::string>());
        auto expected = deployment;
        expected["tensor_parallel"] = participants;
        if (selected.deployment != expected || manifest.framework != expected.at("framework").get<std::string>() || expected.at("runtime") != "ascend")
            throw std::runtime_error("Prediction needs independent models for each target deployment");
        models_paths.emplace(participants, file.get<std::string>());
        costs.emplace(participants, std::move(selected));
    }
    const auto initial = raw.at("kernel_cache_initial_state").get<std::string>();
    if (initial != "empty" && initial != "prepared") throw std::runtime_error("Kernel initial state must be empty or prepared");

    std::ifstream config(manifest.configuration_path.value());
    const auto captured = Json::parse(config);
    if (captured.at("deployment") != raw.at("deployment")) throw std::runtime_error("Base capture belongs to a different deployment");
    const auto & base = captured.at("server").at("arguments");
    if (base.value("disable_radix_cache", false))
        throw std::runtime_error("Regional prediction requires an active radix cache; cache-disabled scheduling is not restored");
    const std::set<std::string> variable{ "page_size",
                                          "max_total_tokens",
                                          "max_prefill_tokens",
                                          "enable_hierarchical_cache",
                                          "hicache_ratio",
                                          "hicache_write_policy",
                                          "hicache_io_backend",
                                          "hicache_mem_layout",
                                          "hicache_storage_backend",
                                          "hicache_storage_prefetch_policy",
                                          "hicache_storage_backend_extra_config",
                                          "hicache_transfer_bandwidth" };
    std::vector<PredictionTarget> targets;
    for (const auto & target : raw.at("targets")) {
        const auto target_deployment = target.at("deployment");
        const auto participants = positive(target_deployment, "tensor_parallel");
        auto expected = deployment;
        expected["tensor_parallel"] = participants;
        if (target_deployment != expected || !costs.contains(participants))
            throw std::runtime_error("Target may only change TP within the declared platform, runtime and model");
        auto configuration = base;
        for (const auto & [key, value] : target.at("configuration").items()) {
            if (key == "tp_size" || key == "model_path" || key == "dtype")
                throw std::runtime_error("Model, dtype and TP belong to target.deployment, not configuration");
            if (!variable.contains(key) && (!base.contains(key) || base.at(key) != value))
                throw std::runtime_error("Target changes an unsupported execution setting: " + key);
            configuration[key] = value;
        }
        // This is the derived native invocation, never a second TP input.
        configuration["tp_size"] = participants;
        const auto page = positive(configuration, "page_size");
        const auto tokens = positive(configuration, "max_total_tokens");
        const auto prefill = positive(configuration, "max_prefill_tokens");
        if (tokens < page || configuration.at("hicache_io_backend") != "kernel_ascend" || configuration.at("hicache_mem_layout") != "page_first_direct")
            throw std::runtime_error("Regional cache prediction needs positive capacity and kernel_ascend/page_first_direct");
        if (configuration.value("hicache_size", 0.0) > 0)
            throw std::runtime_error("Regional prediction currently requires ratio-sized host memory; explicit hicache_size is not restored");
        const auto ratio = configuration.at("hicache_ratio").get<double>();
        const auto host_pages =
            configuration.at("enable_hierarchical_cache").get<bool>() ? sglang::host_capacity_pages((tokens / page) * page, page, ratio) : 1;
        Json settings{
            { "device_pages",                            tokens / page },
            {   "host_pages",                               host_pages },
            { "write_policy", configuration.at("hicache_write_policy") },
            {   "io_backend",   configuration.at("hicache_io_backend") },
            {  "host_layout",   configuration.at("hicache_mem_layout") }
        };
        auto cache = sglang::read_cache_settings(settings);
        cache.bandwidth = sglang::configured_bandwidth(configuration);
        if (!configuration.at("enable_hierarchical_cache").get<bool>()) {
            cache.host_pages = 0;
            cache.backup_after_hits.reset();
        }
        const auto result_implementation =
            configuration.at("enable_hierarchical_cache").get<bool>() ? "sglang.ascend.eager:HiRadixCache" : "sglang.ascend.eager:RadixCache";
        targets.push_back({
            target.at("name").get<std::string>(),
            std::move(configuration),
            target_deployment,
            Json::object(),
            {},
            page,
            { "sglang.ascend.eager",
                        result_implementation, prefill,
                        true, { "cache.load.consumer", "sglang.LayerDoneCounter.wait_until", "caller_blocking_interval" },
                        2, participants,
                        participants == deployment.at("tensor_parallel").get<std::size_t>() },
            std::move(cache)
        });
    }
    auto source = prepare_source(manifest, std::nullopt, SourceDetail::Regions);
    const auto & observations = source.context.observations();
    const auto cache_backend = sglang::cache_backend(source.context);
    const auto source_participants = deployment.at("tensor_parallel").get<std::size_t>();
    std::set<std::size_t> observed_ranks;
    for (const auto & frame : observations.compute_frames) {
        if (!frame.participant || *frame.participant >= source_participants)
            throw std::runtime_error("Base has an unobserved rank identity; capture batch participant metadata before regional prediction");
        observed_ranks.insert(*frame.participant);
    }
    if (observed_ranks.size() != source_participants || cache_backend.participants != source_participants)
        throw std::runtime_error("Base does not contain every declared TP participant");
    for (auto & target : targets) {
        target.backend = cache_backend;
        target.backend.participants = target.deployment.at("tensor_parallel").get<std::size_t>();
        target.work_layout = {
            {             "participants",                                                                 target.backend.participants },
            { "local_kv_bytes_per_token",                                                              target.backend.bytes_per_token },
            { "communication_cost_scope", "Embedded in the target-deployment batch stream response; no additional communication cost" }
        };
        if (!target.regions.transfer_source_control) {
            if (!captured.contains("model_configuration")) throw std::runtime_error("Cross-TP prediction needs model_configuration from the base capture");
            const auto & model = captured.at("model_configuration");
            if (model.at("model_type") != "qwen3" || deployment.at("dtype") != "bfloat16")
                throw std::runtime_error("Cross-TP currently supports Qwen3 dense BF16 execution");
            if (base.value("dp_size", 1) != 1 || base.value("pp_size", 1) != 1 || base.value("enable_dp_attention", false)
                || base.value("enable_dp_lm_head", false) || base.value("enable_lora", false)
                || (base.contains("quantization") && !base.at("quantization").is_null()))
                throw std::runtime_error("Cross-TP requires plain dense TP without DP, PP, LoRA or quantization");
            const auto tp = target.backend.participants;
            if (tp != 1 && tp != 2) throw std::runtime_error("Cross-TP is currently limited to same-host TP=1 or TP=2");
            const auto heads = positive(model, "num_attention_heads");
            const auto kv_heads = positive(model, "num_key_value_heads");
            const auto dim = positive(model, "head_dim");
            if (heads % tp || kv_heads % tp || positive(model, "intermediate_size") % tp || positive(model, "vocab_size") % tp)
                throw std::runtime_error("Target TP must divide Qwen3 attention and feed-forward shards; replicated KV is not restored");
            const auto source_tp = deployment.at("tensor_parallel").get<std::size_t>();
            if (source_tp != 1 && source_tp != 2) throw std::runtime_error("Cross-TP source is currently limited to same-host TP=1 or TP=2");
            const auto bytes = positive(model, "num_hidden_layers") * kv_heads * dim * 2 * 2;
            if (bytes / source_tp != cache_backend.bytes_per_token) throw std::runtime_error("Captured KV geometry disagrees with model_configuration");
            target.backend.bytes_per_token = bytes / tp;
            const auto hidden = positive(model, "hidden_size");
            target.work_layout.update({
                { "local_kv_bytes_per_token", target.backend.bytes_per_token },
                { "layers", positive(model, "num_hidden_layers") },
                { "hidden_size", hidden },
                { "head_dim", dim },
                { "local_query_heads", heads / tp },
                { "local_kv_heads", kv_heads / tp },
                { "local_intermediate_size", positive(model, "intermediate_size") / tp },
                { "qkv_projection_output_width_per_rank", (heads + 2 * kv_heads) * dim / tp },
                { "attention_output_projection_input_width_per_rank", heads * dim / tp },
                { "communication",
                 { { "participants", tp },
                    { "dtype", "bfloat16" },
                    { "embedding_all_reduces_per_phase", tp > 1 ? 1 : 0 },
                    { "layer_all_reduces_per_phase", tp > 1 ? 2 * target.backend.layers : 0 },
                    { "all_reduce_payload_bytes_per_query_token", hidden * 2 },
                    { "logits_all_gathers_per_phase", tp > 1 ? 1 : 0 },
                    { "logits_payload_bytes_per_rank", positive(model, "vocab_size") / tp * (base.value("enable_fp32_lm_head", false) ? 4 : 2) },
                    { "qualification", "Dense Qwen3 eager without DP/sequence parallelism; payload sizes are logical input bytes, not wire traffic" } } }
            });
        }
        if (target.page_tokens > std::numeric_limits<std::size_t>::max() / target.backend.bytes_per_token)
            throw std::runtime_error("KV page bytes exceed the supported size range");
        target.cache.storage = sglang::configured_storage(target.configuration,
                                                          target.cache.device_pages,
                                                          target.cache.host_pages,
                                                          target.page_tokens * target.backend.bytes_per_token);
    }
    if (observations.counts.contains("schedule_input_facts.unsupported"))
        throw std::runtime_error("Regional prediction currently supports plain eager inputs without speculative, multimodal or logprob branches");
    const auto & workload = raw.at("workload");
    if (workload.at("requests").size() != observations.requests.size()) throw std::runtime_error("Requested workload differs from the base capture");
    const auto drain = workload.value("drain_before_formal", false);
    if (observations.workload_barriers.size() != (drain ? 1u : 0u)
        || (drain && observations.workload_barriers.front().before_request != workload.at("formal_begin_index").get<std::size_t>()))
        throw std::runtime_error("Requested HiCache preparation gate differs from the base capture");

    for (auto & target : targets) {
        const auto capacity = target.cache.device_pages * target.page_tokens;
        for (std::size_t id = 0; id < observations.requests.size(); ++id) {
            const auto & tokens = observations.request_tokens.at(observations.requests[id].identity);
            const auto required = sglang::minimum_device_tokens(observations.token_paths.length(tokens.prompt.value().path),
                                                                workload.at("requests").at(id).at("max_new_tokens").get<std::size_t>(),
                                                                target.page_tokens);
            if (capacity >= required) continue;

            target.infeasible = "SGLang request " + std::to_string(id) + " needs at least " + std::to_string(required)
                                + " device tokens including admission reservation; target provides " + std::to_string(capacity);
            break;
        }
    }
    for (std::size_t id = 0; id < observations.requests.size(); ++id) {
        const auto & request = workload.at("requests").at(id);
        const auto & captured_request = observations.requests[id];
        if (request.at("return_logprob").get<bool>()) throw std::runtime_error("Regional prediction does not yet model logprob output work");
        const auto prompt = observations.request_tokens.at(captured_request.identity).prompt.value().path;
        auto requested_tokens = request.at("input_ids");
        if (requested_tokens.is_null()) {
            // Capture records the tokenizer's actual input, including its text
            // origin. Prediction never loads a tokenizer or guesses token IDs.
            const auto & actual = captured.at("workload").at("requests").at(id);
            if (request.at("prompt") != actual.at("prompt")) throw std::runtime_error("Requested text differs from the captured workload");
            requested_tokens = actual.at("input_ids");
        }
        if (requested_tokens.get<std::vector<std::int64_t>>() != observations.token_paths.suffix(prompt, 0))
            throw std::runtime_error("Requested tokens differ from the base's observed workload");
        std::size_t outputs = 0;
        for (const auto frame : source.context.request_frames(captured_request.identity)) {
            const auto * result = source.context.frame_result(frame);
            if (result && result->processed) outputs = std::max(outputs, result->processed->output_tokens_after.front());
        }
        if (!request.at("ignore_eos").get<bool>() || outputs != request.at("max_new_tokens").get<std::size_t>()
            || captured_request.formal != (id >= workload.at("formal_begin_index").get<std::size_t>()))
            throw std::runtime_error("Regional prediction currently requires the same fixed output count and formal workload boundary");
    }
    return { std::move(source),
             deployment,
             std::move(costs),
             std::move(models_paths),
             initial == "empty" ? trace_graph::planning::KernelCacheInitial::Empty : trace_graph::planning::KernelCacheInitial::Prepared,
             std::move(targets),
             std::move(scaling),
             raw.value("record_graph", false) };
}

} // namespace markov::adapters
