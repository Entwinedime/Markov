#include "trace_graph/frontend/model_config.hpp"

#include "trace_graph/core/logger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace TraceGraph {

namespace {

using Json = nlohmann::json;

Json read_json_file(const std::string & filename) {
    /**
     * @brief model config 一般很小，直接用 nlohmann::json DOM 解析，和大 trace 输入路径分开处理。
     */
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
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string json_scalar_to_string(const Json & value, const std::string & def = "") {
    /**
     * @brief 只把 JSON 标量转换成字符串。
     *
     * 复杂对象不会隐式 dump，避免配置结构写错时被静默接受。
     */
    if (value.is_null()) return def;
    if (value.is_string()) return value.get<std::string>();
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    if (value.is_number_unsigned()) return std::to_string(value.get<uint64_t>());
    if (value.is_number_integer()) return std::to_string(value.get<int64_t>());
    if (value.is_number_float()) {
        std::ostringstream os;
        os << value.get<double>();
        return os.str();
    }
    return def;
}

std::string string_value(const Json & object, const std::string & key, const std::string & def) {
    if (!object.is_object()) return def;
    auto it = object.find(key);
    if (it == object.end()) return def;
    return json_scalar_to_string(*it, def);
}

double number_value(const Json & object, const std::string & key, double def) {
    /**
     * @brief 支持数字或字符串数字，便于 Python runner 输出配置时保持宽松。
     */
    if (!object.is_object()) return def;
    auto it = object.find(key);
    if (it == object.end() || it->is_null()) return def;
    if (it->is_number()) return it->get<double>();
    if (it->is_string()) {
        try {
            return std::stod(it->get<std::string>());
        }
        catch (...) {
            return def;
        }
    }
    return def;
}

uint64_t u64_value(const Json & object, const std::string & key, uint64_t def) {
    auto number = number_value(object, key, static_cast<double>(def));
    if (number < 0) return def;
    return static_cast<uint64_t>(number);
}

bool bool_value(const Json & object, const std::string & key, bool def) {
    /**
     * @brief 布尔值支持 true/false 和常见字符串写法。
     */
    if (!object.is_object()) return def;
    auto it = object.find(key);
    if (it == object.end() || it->is_null()) return def;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_string()) {
        auto text = lower(it->get<std::string>());
        if (text == "true" || text == "1" || text == "yes") return true;
        if (text == "false" || text == "0" || text == "no") return false;
    }
    return def;
}

std::vector<std::string> string_array(const Json & root, const std::string & key) {
    /**
     * @brief modules 当前只接受字符串数组。
     *
     * 非字符串项直接忽略，后续可改成 strict fail。
     */
    std::vector<std::string> values;
    if (!root.is_object()) return values;
    auto it = root.find(key);
    if (it == root.end() || !it->is_array()) return values;
    for (const auto & item : *it) {
        if (item.is_string()) values.push_back(item.get<std::string>());
    }
    return values;
}

NodeScaleConfig parse_node_scale(const Json & root, bool module_enabled) {
    /**
     * @brief 解析 NodeScaleModule 配置。
     *
     * 如果 modules 中显式写了 node_scale，但没有 node_scale 对象，则启用空规则模块。
     * 空规则不会修改 DAG，但 summary 能证明模块被调用。
     */
    NodeScaleConfig config;
    auto it = root.find("node_scale");
    if (it == root.end() || !it->is_object()) {
        config.enabled = module_enabled;
        return config;
    }

    const auto & object = *it;
    config.enabled = bool_value(object, "enabled", true);
    auto rules_it = object.find("rules");
    if (rules_it != object.end() && rules_it->is_array()) {
        for (const auto & item : *rules_it) {
            if (!item.is_object()) continue;
            NodeScaleRuleConfig rule;
            rule.id = string_value(item, "id", "");
            rule.name = string_value(item, "name", "");
            rule.factor = number_value(item, "factor", number_value(item, "scale", 1.0));
            if (!rule.name.empty()) config.rules.push_back(rule);
        }
    }
    return config;
}

HiCacheConfig parse_hicache(const Json & root, bool module_enabled) {
    /**
     * @brief 解析 HiCache state 模块的显式 target config。
     *
     * HiCache state prediction 只读取配置事实，不在这里推导策略结果。
     */
    HiCacheConfig config;
    auto it = root.find("hicache");
    if (it == root.end() || !it->is_object()) {
        config.enabled = module_enabled;
        return config;
    }
    const auto & object = *it;
    config.enabled = bool_value(object, "enabled", true);
    config.page_size = u64_value(object, "page_size", 0);
    config.l1_capacity_pages = u64_value(object, "l1_capacity_pages", u64_value(object, "l1_capacity", 0));
    config.l2_capacity_pages = u64_value(object, "l2_capacity_pages", u64_value(object, "l2_capacity", 0));
    config.write_policy = lower(string_value(object, "write_policy", "write_through"));
    if (config.write_policy == "observed") throw std::runtime_error("hicache.write_policy=observed is not supported; use an explicit target write policy");
    if (config.write_policy.empty()) config.write_policy = "write_through";
    config.write_through_threshold = u64_value(object, "write_through_threshold", 0);
    config.prefetch_policy = lower(string_value(object, "prefetch_policy", "timeout"));
    if (config.prefetch_policy == "observed")
        throw std::runtime_error("hicache.prefetch_policy=observed is not supported; use an explicit target prefetch policy");
    if (config.prefetch_policy.empty()) config.prefetch_policy = "timeout";
    config.prefetch_threshold_pages = u64_value(object, "prefetch_threshold_pages", 0);
    config.prefetch_capacity_limit_pages = u64_value(object, "prefetch_capacity_limit_pages", 0);
    const bool has_timeout_base = object.contains("prefetch_timeout_base_sec") || object.contains("prefetch_timeout_base");
    const bool has_timeout_per = object.contains("prefetch_timeout_per_ki_token_sec") || object.contains("prefetch_timeout_per_ki_token");
    const bool has_timeout_max = object.contains("prefetch_timeout_max_sec") || object.contains("prefetch_timeout_max");
    config.prefetch_timeout_configured = has_timeout_base || has_timeout_per || has_timeout_max;
    config.prefetch_timeout_base_sec = number_value(object, "prefetch_timeout_base_sec", number_value(object, "prefetch_timeout_base", 0.0));
    config.prefetch_timeout_per_ki_token_sec =
        number_value(object, "prefetch_timeout_per_ki_token_sec", number_value(object, "prefetch_timeout_per_ki_token", 0.0));
    config.prefetch_timeout_max_sec = number_value(object, "prefetch_timeout_max_sec", number_value(object, "prefetch_timeout_max", 0.0));
    config.emit_state_digests = bool_value(object, "emit_state_digests", false);
    return config;
}

} // namespace

bool ModelConfig::module_enabled(const std::string & name) const { return std::find(modules.begin(), modules.end(), name) != modules.end(); }

ModelConfig ModelConfig::from_file(const std::string & filename) {
    /**
     * @brief C++ 后端只消费窄 model config，不直接理解完整实验配置。
     */
    auto root = read_json_file(filename);
    if (!root.is_object()) { throw std::runtime_error("Model config root must be a JSON object: " + filename); }

    ModelConfig config;
    config.modules = string_array(root, "modules");
    config.node_scale = parse_node_scale(root, config.module_enabled("node_scale"));
    config.hicache = parse_hicache(root, config.module_enabled("hicache"));

    Logger::instance().info() << "Loaded model config from " << filename;
    return config;
}

} // namespace TraceGraph
