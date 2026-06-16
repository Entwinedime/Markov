/**
 * @file
 * @brief HiCache trace event parser 与 token dictionary 解析。
 */
#include "trace_graph/modules/hicache/hicache_fact.hpp"

#include "trace_graph/core/trace_event.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <ranges>
#include <string>

namespace TraceGraph {

namespace {

using Json = nlohmann::json;

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string trim(const std::string & value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

bool false_like(const std::string & value) {
    const auto normalized = lower(trim(value));
    return normalized == "false" || normalized == "0" || normalized == "no" || normalized == "off";
}

bool true_like(const std::string & value) {
    const auto normalized = lower(trim(value));
    return normalized == "true" || normalized == "1" || normalized == "yes" || normalized == "on";
}

bool bool_arg(const TraceEvent & event, const std::string & key, bool fallback) {
    if (!event.has_arg(key)) return fallback;
    const auto value = event.arg(key);
    if (true_like(value)) return true;
    if (false_like(value)) return false;
    return fallback;
}

uint64_t json_u64_value(const Json & object, const std::string & key, uint64_t fallback = 0) {
    if (!object.is_object()) return fallback;
    auto it = object.find(key);
    if (it == object.end() || it->is_null()) return fallback;
    try {
        if (it->is_number_unsigned()) return it->get<uint64_t>();
        if (it->is_number_integer()) {
            auto value = it->get<int64_t>();
            return value >= 0 ? static_cast<uint64_t>(value) : fallback;
        }
        if (it->is_number_float()) {
            auto value = it->get<double>();
            return value >= 0.0 ? static_cast<uint64_t>(value) : fallback;
        }
        if (it->is_string()) {
            auto text = trim(it->get<std::string>());
            if (text.empty() || lower(text) == "none") return fallback;
            auto value = std::stod(text);
            return value >= 0.0 ? static_cast<uint64_t>(value) : fallback;
        }
    }
    catch (...) {
    }
    return fallback;
}

std::string json_string_value(const Json & object, const std::string & key) {
    if (!object.is_object()) return "";
    auto it = object.find(key);
    if (it == object.end() || it->is_null()) return "";
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_unsigned()) return std::to_string(it->get<uint64_t>());
    if (it->is_number_integer()) return std::to_string(it->get<int64_t>());
    if (it->is_boolean()) return it->get<bool>() ? "true" : "false";
    return "";
}

Json parse_json_fragment(const std::string & raw) {
    const auto text = trim(raw);
    if (text.empty()) return Json();
    try {
        auto value = Json::parse(text);
        if (value.is_string()) {
            const auto nested = trim(value.get<std::string>());
            if (!nested.empty() && (nested.front() == '{' || nested.front() == '[')) return Json::parse(nested);
        }
        return value;
    }
    catch (...) {
        return Json();
    }
}

/**
 * @brief 将 JSON 中的 token 表示规范化为 HiCacheToken。
 *
 * parser 接受数字、字符串数字或数组形式；非法 item 被跳过，避免因为单个坏字段中断
 * 整个 trace 的解析。
 */
HiCacheToken token_from_json(const Json & value) {
    HiCacheToken token;
    auto append_word = [&](const Json & item) {
        try {
            if (item.is_number_unsigned()) token.words.push_back(static_cast<uint32_t>(item.get<uint64_t>()));
            else if (item.is_number_integer()) {
                auto number = item.get<int64_t>();
                if (number >= 0) token.words.push_back(static_cast<uint32_t>(number));
            }
            else if (item.is_string()) {
                auto number = std::stoll(item.get<std::string>());
                if (number >= 0) token.words.push_back(static_cast<uint32_t>(number));
            }
        }
        catch (...) {
        }
    };

    if (value.is_array()) {
        for (const auto & item : value) append_word(item);
    }
    else { append_word(value); }
    return token;
}

/** @brief 从 JSON array 构造 token path，并丢弃无法解析的 token。 */
HiCacheTokenPath token_path_from_json(const Json & value) {
    HiCacheTokenPath path;
    if (!value.is_array()) return path;
    std::ranges::for_each(value, [&](const auto & item) {
        auto token = token_from_json(item);
        if (!token.words.empty()) path.push_back(std::move(token));
    });
    return path;
}

HiCacheTokenPath slice_tokens(const HiCacheTokenPath & tokens, uint64_t begin, uint64_t end) {
    if (begin >= tokens.size() || begin >= end) return {};
    end = std::min<uint64_t>(end, tokens.size());
    HiCacheTokenPath result;
    result.reserve(static_cast<size_t>(end - begin));
    auto slice = tokens | std::views::drop(static_cast<std::ranges::range_difference_t<HiCacheTokenPath>>(begin))
                 | std::views::take(static_cast<std::ranges::range_difference_t<HiCacheTokenPath>>(end - begin));
    std::ranges::copy(slice, std::back_inserter(result));
    return result;
}

} // namespace

/**
 * @brief 识别 HiCache event 域。
 *
 * 这里兼容 target_id、domain/category 和事件名前缀三类探针输出形态；识别事件不等于
 * 允许其进入状态模型，后续仍由 router 做 invariant 输入契约检查。
 */
bool HiCacheFactParser::is_hicache_event(const TraceEvent & event) const {
    const auto target_id = lower(event.arg("target_id"));
    if (target_id.starts_with("hiradix.") || target_id.starts_with("hicache.") || target_id.starts_with("hicache_controller.")) return true;
    const auto domain = lower(event.arg("domain"));
    if (domain == "python_probe" && lower(event.name).contains("hicache")) return true;
    if (event.cat == "hicache") return true;
    return event.name.starts_with("HiCache::") || event.name.starts_with("hicache_");
}

/** @brief 在第一轮扫描中收集 token dictionary。 */
void HiCacheFactParser::observe_token_dictionaries(const TraceEvent & event) {
    if (!is_hicache_event(event)) return;
    std::ranges::for_each(event.args, [&](const auto & item) {
        const auto & [key, raw] = item;
        if (!key.contains("dictionary")) return;
        observe_dictionary_value(raw);
    });
}

/**
 * @brief 解析并保存一个 dictionary JSON 片段。
 *
 * dictionary 只提供 token_path_id 到 token_ids 的映射，不携带 page residency 语义。
 */
void HiCacheFactParser::observe_dictionary_value(const std::string & raw) {
    auto value = parse_json_fragment(raw);
    if (!value.is_object()) return;
    auto path_id = json_string_value(value, "token_path_id");
    if (path_id.empty()) path_id = json_string_value(value, "path_id");
    if (path_id.empty()) return;
    auto ids = value.find("token_ids");
    if (ids == value.end() || !ids->is_array()) return;
    token_paths_[path_id] = token_path_from_json(*ids);
}

/** @brief 从 event arg 中解析 token span 元数据。 */
HiCacheTokenSpan HiCacheFactParser::parse_span(const TraceEvent & event, const std::string & key) const {
    HiCacheTokenSpan span;
    auto value = parse_json_fragment(event.arg(key));
    if (!value.is_object()) return span;
    span.path_id = json_string_value(value, "path_id");
    if (span.path_id.empty()) span.path_id = json_string_value(value, "token_path_id");
    span.begin = json_u64_value(value, "begin", 0);
    span.end = json_u64_value(value, "end", 0);
    span.token_count = json_u64_value(value, "token_count", span.end > span.begin ? span.end - span.begin : 0);
    span.hash_algo = json_string_value(value, "hash_algo");
    span.valid = !span.path_id.empty() && span.end >= span.begin;
    return span;
}

/**
 * @brief 使用已观察 dictionary 解析 span。
 *
 * dictionary 不存在或区间非法时返回空 path，让上层记录缺失 invariant。
 */
HiCacheTokenPath HiCacheFactParser::resolve_span(const HiCacheTokenSpan & span) const {
    if (!span.valid) return {};
    auto it = token_paths_.find(span.path_id);
    if (it == token_paths_.end()) return {};
    return slice_tokens(it->second, span.begin, span.end);
}

/**
 * @brief 将 TraceEvent 转换为 HiCacheFact。
 *
 * parse 只做字段规范化和 token span 解析，不判断 role 是否可建模，也不把 source
 * evidence 转换成 target state。
 */
HiCacheFact HiCacheFactParser::parse(size_t node_id, const TraceEvent & event) const {
    HiCacheFact fact;
    fact.source_node_id = node_id;
    fact.source_event_index = event.index;
    fact.ts = event.ts;
    fact.dur = event.dur;
    fact.event_name = event.name;
    fact.target_id = event.arg("target_id");
    fact.fact_class = event.arg("fact_class");
    fact.fact_granularity = event.arg("fact_granularity");
    fact.role = event.arg("event_role");
    if (fact.role.empty()) fact.role = "unknown";
    fact.phase = event.arg("phase");
    if (fact.phase.empty()) {
        if (event.name.ends_with("_start")) fact.phase = "start";
        else if (event.name.ends_with("_end")) fact.phase = "end";
    }
    fact.is_start = fact.phase == "start" || event.name.ends_with("_start");
    fact.is_end = fact.phase == "end" || event.name.ends_with("_end");
    fact.request_id = event.arg("request_id");
    fact.operation_id = event.arg("operation_id", event.arg("node_id"));
    fact.cache_scope = event.arg("cache_scope", event.pid.empty() ? "-1" : event.pid);
    fact.check_kind = event.arg("check_kind", event.arg("checkpoint_kind"));
    fact.lifecycle_kind = event.arg("lifecycle_kind");
    fact.admission_kind = event.arg("admission_kind");
    fact.seq_no = event.arg_u64("seq_no", 0);
    fact.source_page_size = event.arg_u64("source_page_size", event.arg_u64("page_size", 0));
    fact.token_count = event.arg_u64("token_count", 0);
    fact.max_new_tokens = event.arg_u64("max_new_tokens", 0);
    fact.truncation_align_size = event.arg_u64("truncation_align_size", 0);
    fact.priority = static_cast<int64_t>(event.arg_u64("priority", 0));
    fact.has_chunked_req = bool_arg(event, "has_chunked_req", false);
    fact.ignore_eos = bool_arg(event, "ignore_eos", false);
    fact.model_input = bool_arg(event, "model_input", false);
    fact.dag_input = bool_arg(event, "dag_input", false);

    fact.full_path_span = parse_span(event, "full_path_span");

    fact.full_path_tokens = resolve_span(fact.full_path_span);
    return fact;
}

} // namespace TraceGraph
