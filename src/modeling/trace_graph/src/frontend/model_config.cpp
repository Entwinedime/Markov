/**
 * @file
 * @brief Narrow C++ model-configuration parser implementation.
 */
#include "markov/trace_graph/frontend/model_config.hpp"

#include "markov/trace_graph/core/logger.hpp"
#include "markov/trace_graph/core/numeric.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <string>

namespace markov::trace_graph::frontend {

namespace model_config_detail {

using Json = nlohmann::json;

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

std::string string_value(const Json & object, const std::string & key, const std::string & def) {
    if (!object.is_object()) return def;
    auto it = object.find(key);
    if (it == object.end() || it->is_null()) return def;
    if (!it->is_string()) throw std::runtime_error("Model config field '" + key + "' must be a string");
    return it->get<std::string>();
}

double number_value(const Json & object, const std::string & key, double def) {
    if (!object.is_object()) return def;
    auto it = object.find(key);
    if (it == object.end() || it->is_null()) return def;
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
    if (!object.is_object()) return def;
    auto it = object.find(key);
    if (it == object.end() || it->is_null()) return def;
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
    if (!object.is_object()) return def;
    auto it = object.find(key);
    if (it == object.end() || it->is_null()) return def;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_string()) {
        auto text = lower(it->get<std::string>());
        if (text == "true" || text == "1" || text == "yes") return true;
        if (text == "false" || text == "0" || text == "no") return false;
    }
    throw std::runtime_error("Model config field '" + key + "' must be a boolean");
}

NodeScaleConfig parse_node_scale(const Json & root) {
    // Activation is owned by this object; there is no second module registry to reconcile.
    NodeScaleConfig config;
    auto it = root.find("node_scale");
    if (it == root.end() || it->is_null()) return config;
    if (!it->is_object()) throw std::runtime_error("Model config field 'node_scale' must be an object");

    const auto & object = *it;
    config.enabled = bool_value(object, "enabled", true);
    auto rules_it = object.find("rules");
    if (rules_it != object.end() && !rules_it->is_null()) {
        if (!rules_it->is_array()) throw std::runtime_error("Model config field 'node_scale.rules' must be an array");
        config.rules.reserve(rules_it->size());
        for (const auto & item : *rules_it) {
            if (!item.is_object()) throw std::runtime_error("Each node_scale rule must be an object");
            NodeScaleRuleConfig rule;
            rule.id = string_value(item, "id", "");
            rule.name = string_value(item, "name", "");
            rule.factor = number_value(item, "factor", 1.0);
            if (rule.name.empty()) throw std::runtime_error("Each node_scale rule requires a non-empty name");
            if (rule.factor <= 0.0) throw std::runtime_error("Each node_scale rule requires factor > 0");
            config.rules.push_back(std::move(rule));
        }
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
#ifdef DEBUG
    config.emit_state_digests = bool_value(object, "emit_state_digests", false);
#endif
    if (const auto dag_patch = object.find("dag_patch"); dag_patch != object.end() && !dag_patch->is_null()) {
        if (!dag_patch->is_object()) throw std::runtime_error("Model config field 'hicache.dag_patch' must be an object");
        config.dag_patch_enabled = bool_value(*dag_patch, "enabled", false);
    }
    return config;
}

} // namespace model_config_detail

using model_config_detail::parse_hicache;
using model_config_detail::parse_node_scale;
using model_config_detail::read_json_file;

ModelConfig ModelConfig::from_file(const std::string & filename) {
    // The backend accepts only this narrow schema, not the complete experiment document.
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
