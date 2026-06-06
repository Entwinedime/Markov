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
    // model config 一般很小，直接用 nlohmann::json DOM 解析，和大 trace 输入路径分开处理。
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
    // 只把 JSON 标量转换成字符串；复杂对象不会隐式 dump，避免配置结构写错时被静默接受。
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
    // 支持数字或字符串数字，便于 Python runner 输出配置时保持宽松。
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

bool bool_value(const Json & object, const std::string & key, bool def) {
    // 布尔值支持 true/false 和常见字符串写法。
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
    // modules 当前只接受字符串数组；非字符串项直接忽略，后续可改成 strict fail。
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
    // 如果 modules 中显式写了 node_scale，但没有 node_scale 对象，则启用空规则模块。
    // 空规则不会修改 DAG，但 summary 能证明模块被调用。
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
    // HiCache 当前先解析 enabled 开关；page size、capacity、policy 等参数后续从这里扩展。
    HiCacheConfig config;
    auto it = root.find("hicache");
    if (it == root.end() || !it->is_object()) {
        config.enabled = module_enabled;
        return config;
    }
    config.enabled = bool_value(*it, "enabled", true);
    return config;
}

} // namespace

bool ModelConfig::module_enabled(const std::string & name) const {
    return std::find(modules.begin(), modules.end(), name) != modules.end();
}

ModelConfig ModelConfig::from_file(const std::string & filename) {
    // C++ 后端只消费窄 model config，不直接理解完整实验配置。
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
