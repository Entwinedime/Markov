#include "trace_graph/frontend/model_config.hpp"

#include "trace_graph/logger.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace TraceGraph {

namespace {

std::string read_file(const std::string & filename) {
    std::ifstream ifs(filename);
    if (!ifs.is_open()) { throw std::runtime_error("Failed to open model config: " + filename); }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool contains_string(const std::string & text, const std::string & value) {
    return text.find("\"" + value + "\"") != std::string::npos;
}

std::string string_value(const std::string & text, const std::string & key, const std::string & def) {
    std::regex re("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch m;
    if (std::regex_search(text, m, re)) return m[1].str();
    return def;
}

double number_value(const std::string & text, const std::string & key, double def) {
    std::regex re("\"" + key + "\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?)");
    std::smatch m;
    if (std::regex_search(text, m, re)) return std::stod(m[1].str());
    return def;
}

size_t find_matching(const std::string & text, size_t open_pos, char open_ch, char close_ch) {
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = open_pos; i < text.size(); ++i) {
        char c = text[i];
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        else if (c == open_ch) depth++;
        else if (c == close_ch) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return std::string::npos;
}

std::string object_for_key(const std::string & text, const std::string & key) {
    auto key_pos = text.find("\"" + key + "\"");
    if (key_pos == std::string::npos) return {};
    auto open_pos = text.find('{', key_pos);
    if (open_pos == std::string::npos) return {};
    auto close_pos = find_matching(text, open_pos, '{', '}');
    if (close_pos == std::string::npos) return {};
    return text.substr(open_pos, close_pos - open_pos + 1);
}

std::string array_for_key(const std::string & text, const std::string & key) {
    auto key_pos = text.find("\"" + key + "\"");
    if (key_pos == std::string::npos) return {};
    auto open_pos = text.find('[', key_pos);
    if (open_pos == std::string::npos) return {};
    auto close_pos = find_matching(text, open_pos, '[', ']');
    if (close_pos == std::string::npos) return {};
    return text.substr(open_pos, close_pos - open_pos + 1);
}

std::vector<std::string> string_array(const std::string & text, const std::string & key) {
    std::vector<std::string> values;
    auto array = array_for_key(text, key);
    std::regex re("\"([^\"]*)\"");
    for (std::sregex_iterator it(array.begin(), array.end(), re), end; it != end; ++it) { values.push_back((*it)[1].str()); }
    return values;
}

void parse_capacity(const std::string & object, CacheIOTierConfig & tier) {
    auto value = string_value(object, "capacity_pages", "");
    if (value.empty()) {
        double numeric = number_value(object, "capacity_pages", -1.0);
        if (numeric >= 0) {
            tier.capacity_infer = false;
            tier.capacity_pages = static_cast<uint64_t>(numeric);
        }
        return;
    }
    value = lower(value);
    tier.capacity_infer = value == "infer";
    tier.capacity_infinite = value == "infinite" || value == "inf";
}

void parse_bandwidth(const std::string & object, CacheIOTierConfig & tier) {
    auto value = string_value(object, "bandwidth_gbps", "");
    if (value.empty()) {
        double numeric = number_value(object, "bandwidth_gbps", -1.0);
        if (numeric >= 0) {
            tier.bandwidth_infer = false;
            tier.bandwidth_gbps = numeric;
        }
        return;
    }
    value = lower(value);
    tier.bandwidth_infer = value == "infer";
    tier.bandwidth_infinite = value == "infinite" || value == "inf";
}

std::vector<CacheIOTierConfig> parse_tiers(const std::string & cache_object) {
    std::vector<CacheIOTierConfig> tiers;
    auto array = array_for_key(cache_object, "tiers");
    size_t pos = 0;
    while (true) {
        auto open_pos = array.find('{', pos);
        if (open_pos == std::string::npos) break;
        auto close_pos = find_matching(array, open_pos, '{', '}');
        if (close_pos == std::string::npos) break;
        auto object = array.substr(open_pos, close_pos - open_pos + 1);
        CacheIOTierConfig tier;
        tier.name = string_value(object, "name", "");
        parse_capacity(object, tier);
        tier.latency_us = number_value(object, "latency_us", 0.0);
        parse_bandwidth(object, tier);
        tier.eviction = string_value(object, "eviction", "lru");
        if (!tier.name.empty()) tiers.push_back(tier);
        pos = close_pos + 1;
    }
    return tiers;
}

} // namespace

bool ModelConfig::domain_enabled(const std::string & name) const {
    return std::find(domains.begin(), domains.end(), name) != domains.end();
}

ModelConfig ModelConfig::from_file(const std::string & filename) {
    auto text = read_file(filename);
    ModelConfig config;
    config.domains = string_array(text, "domains");

    auto cache_object = object_for_key(text, "cache_io");
    if (!cache_object.empty()) {
        config.cache_io.enabled = contains_string(text, "cache_io") || config.domain_enabled("cache_io");
        config.cache_io.page_size_tokens = string_value(cache_object, "page_size_tokens", "infer");
        config.cache_io.bytes_per_page = string_value(cache_object, "bytes_per_page", "infer");
        config.cache_io.write_policy = string_value(cache_object, "write_policy", "trace");
        config.cache_io.prefetch_policy = string_value(cache_object, "prefetch_policy", "trace_replay");
        config.cache_io.tiers = parse_tiers(cache_object);
    }

    if (config.domain_enabled("cache_io") && !config.cache_io.enabled) config.cache_io.enabled = true;
    Logger::instance().info() << "Loaded model config from " << filename;
    return config;
}

} // namespace TraceGraph
