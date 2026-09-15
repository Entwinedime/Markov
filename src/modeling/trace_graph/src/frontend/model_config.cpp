/**
 * @file
 * @brief Narrow C++ model-configuration parser implementation.
 */
#include "markov/trace_graph/frontend/model_config.hpp"

#include "model_config_parse_detail.hpp"

#include "markov/trace_graph/core/logger.hpp"
#include "markov/trace_graph/core/numeric.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

namespace markov::trace_graph::frontend {

namespace model_config_detail {

Json read_json_file(const std::string & filename) {
    // Model configurations are small; a DOM keeps this boundary strict and readable without
    // affecting the streaming parser used for multi-gigabyte traces.
    std::ifstream ifs(filename);
    if (!ifs.is_open()) { throw std::runtime_error("Failed to open model config: " + filename); }
    try {
        return Json::parse(ifs);
    }
    catch (const std::exception & e) {
        throw std::runtime_error("Failed to parse model config JSON '" + filename + "': " + e.what());
    }
}

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool is_allowed_policy(const std::string & value, std::initializer_list<const char *> allowed) { return std::ranges::find(allowed, value) != allowed.end(); }

void require_known_fields(const Json & object, const std::string & context, std::initializer_list<const char *> allowed) {
    for (const auto & [name, value] : object.items()) {
        (void)value;
        const bool known = std::ranges::any_of(allowed, [&](const char * field) { return name == field; });
        if (!known) throw std::runtime_error("Unknown model config field '" + context + "." + name + "'");
    }
}

void require_exact_fields(const Json & object, const std::string & context, std::initializer_list<const char *> required) {
    require_known_fields(object, context, required);
    if (object.size() != required.size()) throw std::runtime_error("Model config field '" + context + "' requires every canonical field");
}

std::string string_value(const Json & object, const std::string & key, const std::string & def) {
    auto it = object.find(key);
    if (it == object.end()) return def;
    if (!it->is_string()) throw std::runtime_error("Model config field '" + key + "' must be a string");
    return it->get<std::string>();
}

double number_value(const Json & object, const std::string & key, double def) {
    auto it = object.find(key);
    if (it == object.end()) return def;
    std::optional<double> value;
    if (it->is_number()) {
        const auto numeric = it->get<double>();
        if (std::isfinite(numeric)) value = numeric;
    }
    else if (it->is_string()) value = core::parse_finite_double(it->get_ref<const std::string &>());
    if (!value) throw std::runtime_error("Model config field '" + key + "' must be a finite number");
    return *value;
}

uint64_t u64_value(const Json & object, const std::string & key, uint64_t def) {
    auto it = object.find(key);
    if (it == object.end()) return def;
    if (it->is_number_unsigned()) return it->get<uint64_t>();
    if (it->is_number_integer()) {
        const auto value = it->get<int64_t>();
        if (value >= 0) return static_cast<uint64_t>(value);
    }
    else if (it->is_number_float()) {
        if (const auto value = core::truncate_to_u64(it->get<double>())) return *value;
    }
    else if (it->is_string()) {
        if (const auto value = core::parse_u64(it->get_ref<const std::string &>())) return *value;
    }
    throw std::runtime_error("Model config field '" + key + "' must be a non-negative uint64-compatible number");
}

bool bool_value(const Json & object, const std::string & key, bool def) {
    auto it = object.find(key);
    if (it == object.end()) return def;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_string()) {
        auto text = lower(it->get<std::string>());
        if (text == "true" || text == "1" || text == "yes") return true;
        if (text == "false" || text == "0" || text == "no") return false;
    }
    throw std::runtime_error("Model config field '" + key + "' must be a boolean");
}

std::map<std::string, std::string> string_map_value(const Json & object, const std::string & key) {
    const auto it = object.find(key);
    if (it == object.end()) return {};
    if (!it->is_object()) throw std::runtime_error("Model config field '" + key + "' must be an object");
    std::map<std::string, std::string> values;
    for (const auto & [name, value] : it->items()) {
        if (!value.is_string()) throw std::runtime_error("Model config field '" + key + "." + name + "' must be a string");
        values.emplace(name, value.get<std::string>());
    }
    return values;
}

NodeScaleConfig parse_node_scale(const Json & root) {
    NodeScaleConfig config;
    const auto it = root.find("node_scale");
    if (it == root.end() || it->is_null()) return config;
    if (!it->is_object()) throw std::runtime_error("Model config field 'node_scale' must be an object");

    config.enabled = bool_value(*it, "enabled", true);
    const auto rules = it->find("rules");
    if (rules == it->end() || rules->is_null()) return config;
    if (!rules->is_array()) throw std::runtime_error("Model config field 'node_scale.rules' must be an array");

    config.rules.reserve(rules->size());
    for (const auto & item : *rules) {
        if (!item.is_object()) throw std::runtime_error("Each node_scale rule must be an object");
        NodeScaleRuleConfig rule{
            .name = string_value(item, "name", ""),
            .factor = number_value(item, "factor", 1.0),
        };
        if (rule.name.empty()) throw std::runtime_error("Each node_scale rule requires a non-empty name");
        if (rule.factor <= 0.0) throw std::runtime_error("Each node_scale rule requires factor > 0");
        config.rules.push_back(std::move(rule));
    }
    return config;
}

HiCacheConfig parse_hicache(const Json & root) {
    // Parsing validates explicit target facts. Policy derivation remains in HiCachePolicy.
    HiCacheConfig config{};
    auto it = root.find("hicache");
    if (it == root.end() || it->is_null()) return config;
    if (!it->is_object()) throw std::runtime_error("Model config field 'hicache' must be an object");
    const auto & object = *it;
    config.enabled = bool_value(object, "enabled", true);
    config.page_size = u64_value(object, "page_size", 0);
    config.kv_bytes_per_page = u64_value(object, "kv_bytes_per_page", 0);
    config.l1_capacity_pages = u64_value(object, "l1_capacity_pages", 0);
    config.l2_capacity_pages = u64_value(object, "l2_capacity_pages", 0);
    config.write_policy = lower(string_value(object, "write_policy", "write_through"));
    if (config.write_policy == "observed") throw std::runtime_error("hicache.write_policy=observed is not supported; use an explicit target write policy");
    if (config.write_policy.empty()) config.write_policy = "write_through";
    if (!is_allowed_policy(config.write_policy, { "write_through", "write_through_selective", "write_back" }))
        throw std::runtime_error("Invalid hicache.write_policy: " + config.write_policy);
    config.write_through_threshold = u64_value(object, "write_through_threshold", 0);
    config.prefetch_policy = lower(string_value(object, "prefetch_policy", "timeout"));
    if (config.prefetch_policy == "observed")
        throw std::runtime_error("hicache.prefetch_policy=observed is not supported; use an explicit target prefetch policy");
    if (config.prefetch_policy.empty()) config.prefetch_policy = "timeout";
    config.prefetch_threshold_pages = u64_value(object, "prefetch_threshold_pages", 0);
    config.prefetch_capacity_limit_pages = u64_value(object, "prefetch_capacity_limit_pages", 0);
    const bool has_timeout_base = object.contains("prefetch_timeout_base_sec");
    const bool has_timeout_per = object.contains("prefetch_timeout_per_ki_token_sec");
    const bool has_timeout_max = object.contains("prefetch_timeout_max_sec");
    if ((has_timeout_base || has_timeout_per || has_timeout_max) && !(has_timeout_base && has_timeout_per && has_timeout_max)) {
        throw std::runtime_error("HiCache prefetch timeout config requires base, per_ki_token, and max fields together");
    }
    config.prefetch_timeout_configured = has_timeout_base || has_timeout_per || has_timeout_max;
    config.prefetch_timeout_base_sec = number_value(object, "prefetch_timeout_base_sec", 0.0);
    config.prefetch_timeout_per_ki_token_sec = number_value(object, "prefetch_timeout_per_ki_token_sec", 0.0);
    config.prefetch_timeout_max_sec = number_value(object, "prefetch_timeout_max_sec", 0.0);
    if (config.prefetch_timeout_base_sec < 0.0 || config.prefetch_timeout_per_ki_token_sec < 0.0 || config.prefetch_timeout_max_sec < 0.0) {
        throw std::runtime_error("HiCache prefetch timeout fields must be non-negative");
    }
    const auto disaggregation_mode = lower(string_value(object, "disaggregation_mode", ""));
    config.device_allocator_need_sort = bool_value(object, "device_allocator_need_sort", disaggregation_mode == "decode" || disaggregation_mode == "prefill");
    config.io_cost = parse_hicache_io_cost(object);
    if (const auto phase_cost = object.find("phase_cost"); phase_cost != object.end() && !phase_cost->is_null()) {
        if (!phase_cost->is_object()) throw std::runtime_error("hicache.phase_cost must be an object");
        require_exact_fields(*phase_cost,
                             "hicache.phase_cost",
                             { "prefill_common_kernel", "prefill_collective", "prefill_prefix_attention", "decode_paged_attention", "decode_collective", "coverage" });
        const auto parse_component = [&](const char * name) {
            const auto item = phase_cost->find(name);
            if (item == phase_cost->end() || !item->is_object()) throw std::runtime_error(std::string("hicache.phase_cost.") + name + " must be an object");
            require_exact_fields(*item,
                                 std::string("hicache.phase_cost.") + name,
                                 { "fixed_us", "per_new_token_us", "per_attention_token_pair_us", "per_context_token_us" });
            HiCachePhaseLinearCostConfig value{
                .fixed_us = number_value(*item, "fixed_us", 0.0),
                .per_new_token_us = number_value(*item, "per_new_token_us", 0.0),
                .per_attention_token_pair_us = number_value(*item, "per_attention_token_pair_us", 0.0),
                .per_context_token_us = number_value(*item, "per_context_token_us", 0.0),
            };
            if (value.fixed_us < 0.0 || value.per_new_token_us < 0.0 || value.per_attention_token_pair_us < 0.0
                || value.per_context_token_us < 0.0)
                throw std::runtime_error(std::string("HiCache phase coefficients must be non-negative for ") + name);
            return value;
        };
        const auto parse_token_curve = [&](const char * name) {
            const auto item = phase_cost->find(name);
            if (item == phase_cost->end() || !item->is_object())
                throw std::runtime_error(std::string("hicache.phase_cost.") + name + " must be an object");
            require_exact_fields(*item, std::string("hicache.phase_cost.") + name, { "new_token_points" });
            const auto points = item->find("new_token_points");
            if (points == item->end() || !points->is_array() || points->size() < 2)
                throw std::runtime_error(std::string("HiCache phase token curve requires two anchors for ") + name);
            HiCachePhaseTokenCostConfig value;
            uint64_t previous_tokens = 0;
            double previous_duration = 0.0;
            for (const auto & point : *points) {
                require_exact_fields(point, std::string("hicache.phase_cost.") + name + ".new_token_points",
                                     { "new_tokens", "duration_us" });
                const auto tokens = u64_value(point, "new_tokens", 0);
                const auto duration = number_value(point, "duration_us", 0.0);
                if (tokens <= previous_tokens || duration <= 0.0 || duration < previous_duration)
                    throw std::runtime_error(std::string("HiCache phase token anchors must increase for ") + name);
                value.points.push_back({ .new_tokens = tokens, .duration_us = duration });
                previous_tokens = tokens;
                previous_duration = duration;
            }
            return value;
        };
        config.phase_cost.enabled = true;
        config.phase_cost.prefill_common_kernel = parse_token_curve("prefill_common_kernel");
        config.phase_cost.prefill_collective = parse_token_curve("prefill_collective");
        config.phase_cost.prefill_prefix_attention = parse_component("prefill_prefix_attention");
        const auto decode_paged_attention = phase_cost->find("decode_paged_attention");
        if (decode_paged_attention == phase_cost->end() || !decode_paged_attention->is_object())
            throw std::runtime_error("hicache.phase_cost.decode_paged_attention must be an object");
        require_exact_fields(*decode_paged_attention,
                             "hicache.phase_cost.decode_paged_attention",
                             { "kernel_page_tokens", "fixed_us_per_iteration", "per_context_token_us", "per_effective_page_us" });
        config.phase_cost.decode_paged_attention = HiCacheDecodePagedAttentionCostConfig{
            .kernel_page_tokens = u64_value(*decode_paged_attention, "kernel_page_tokens", 0),
            .fixed_us_per_iteration = number_value(*decode_paged_attention, "fixed_us_per_iteration", 0.0),
            .per_context_token_us = number_value(*decode_paged_attention, "per_context_token_us", 0.0),
            .per_effective_page_us = number_value(*decode_paged_attention, "per_effective_page_us", 0.0),
        };
        if (config.phase_cost.decode_paged_attention.kernel_page_tokens == 0
            || config.phase_cost.decode_paged_attention.fixed_us_per_iteration < 0.0
            || config.phase_cost.decode_paged_attention.per_context_token_us < 0.0
            || config.phase_cost.decode_paged_attention.per_effective_page_us < 0.0)
            throw std::runtime_error("HiCache Decode paged-attention fields must be non-negative and kernel_page_tokens positive");
        config.phase_cost.decode_collective = parse_component("decode_collective");
        const auto coverage = phase_cost->find("coverage");
        if (coverage == phase_cost->end() || !coverage->is_object()) throw std::runtime_error("hicache.phase_cost.coverage must be an object");
        require_exact_fields(*coverage,
                             "hicache.phase_cost.coverage",
                             { "min_new_tokens", "max_new_tokens", "min_context_tokens", "max_context_tokens", "min_attention_token_pairs", "max_attention_token_pairs", "min_decode_context_tokens", "max_decode_context_tokens", "base_page_size" });
        config.phase_cost.min_new_tokens = u64_value(*coverage, "min_new_tokens", 0);
        config.phase_cost.max_new_tokens = u64_value(*coverage, "max_new_tokens", 0);
        config.phase_cost.min_context_tokens = u64_value(*coverage, "min_context_tokens", 0);
        config.phase_cost.max_context_tokens = u64_value(*coverage, "max_context_tokens", 0);
        config.phase_cost.min_attention_token_pairs = number_value(*coverage, "min_attention_token_pairs", 0.0);
        config.phase_cost.max_attention_token_pairs = number_value(*coverage, "max_attention_token_pairs", 0.0);
        config.phase_cost.min_decode_context_tokens = u64_value(*coverage, "min_decode_context_tokens", 0);
        config.phase_cost.max_decode_context_tokens = u64_value(*coverage, "max_decode_context_tokens", 0);
        config.phase_cost.base_page_size = u64_value(*coverage, "base_page_size", 0);
        if (config.phase_cost.min_new_tokens > config.phase_cost.max_new_tokens
            || config.phase_cost.min_context_tokens > config.phase_cost.max_context_tokens
            || config.phase_cost.min_attention_token_pairs > config.phase_cost.max_attention_token_pairs
            || config.phase_cost.min_decode_context_tokens > config.phase_cost.max_decode_context_tokens)
            throw std::runtime_error("HiCache phase coverage minima must not exceed maxima");
        if (config.phase_cost.base_page_size == 0)
            throw std::runtime_error("HiCache phase selected-base page size must be positive");
    }
    if (const auto dag_patch = object.find("dag_patch"); dag_patch != object.end() && !dag_patch->is_null()) {
        if (!dag_patch->is_object()) throw std::runtime_error("Model config field 'hicache.dag_patch' must be an object");
        config.dag_patch_enabled = bool_value(*dag_patch, "enabled", false);
        config.dag_patch_source_target_same_config = bool_value(*dag_patch, "source_target_same_config", false);
    }
    return config;
}

} // namespace model_config_detail

using model_config_detail::parse_hicache;
using model_config_detail::parse_node_scale;
using model_config_detail::read_json_file;

ModelConfig ModelConfig::from_file(const std::string & filename) {
    // The backend accepts only this narrow input contract, not the complete experiment document.
    auto root = read_json_file(filename);
    if (!root.is_object()) { throw std::runtime_error("Model config root must be a JSON object: " + filename); }

    ModelConfig config;
    config.node_scale = parse_node_scale(root);
    config.hicache = parse_hicache(root);

    auto & logger = core::Logger::instance();
    if (logger.enabled(core::Logger::Info)) logger.info() << "Loaded model config from " << filename;
    return config;
}

} // namespace markov::trace_graph::frontend
