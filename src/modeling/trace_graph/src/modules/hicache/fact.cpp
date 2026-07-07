/**
 * @file
 * @brief HiCache trace fact parser 与 token dictionary 解析。
 */
#include "markov/trace_graph/modules/hicache/fact.hpp"

#include "markov/trace_graph/core/trace_event.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>

namespace markov::trace_graph::modules::hicache {

using core::TraceEvent;

namespace fact_detail {

using Json = nlohmann::json;

std::string trim_copy(const std::string & text) {
    auto begin = text.begin();
    while (begin != text.end() && std::isspace(static_cast<unsigned char>(*begin))) ++begin;
    auto end = text.end();
    while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1)))) --end;
    return std::string(begin, end);
}

std::string lower_copy(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

bool state_model_dictionary_phase(const TraceEvent & event, const std::string & fact_class, const std::string & role) {
    if (fact_class != "workload_identity") return false;
    const auto phase = lower_copy(trim_copy(event.arg("phase")));
    if (role == "cache_extend_input") return phase == "start";
    return phase == "end";
}

Json parse_json_arg(const std::string & raw) {
    const auto text = trim_copy(raw);
    if (text.empty()) return Json{};
    try {
        auto parsed = Json::parse(text);
        if (parsed.is_string()) {
            const auto nested = trim_copy(parsed.get<std::string>());
            if (!nested.empty() && (nested.front() == '{' || nested.front() == '[')) return Json::parse(nested);
        }
        return parsed;
    }
    catch (...) {
        return Json{};
    }
}

std::string json_string(const Json & object, const std::string & key) {
    if (!object.is_object()) return "";
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) return "";
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_unsigned()) return std::to_string(it->get<uint64_t>());
    if (it->is_number_integer()) return std::to_string(it->get<int64_t>());
    if (it->is_boolean()) return it->get<bool>() ? "true" : "false";
    return "";
}

uint64_t json_u64(const Json & object, const std::string & key, uint64_t fallback = 0) {
    if (!object.is_object()) return fallback;
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) return fallback;
    try {
        if (it->is_number_unsigned()) return it->get<uint64_t>();
        if (it->is_number_integer()) {
            const auto value = it->get<int64_t>();
            return value >= 0 ? static_cast<uint64_t>(value) : fallback;
        }
        if (it->is_number_float()) {
            const auto value = it->get<double>();
            return value >= 0.0 ? static_cast<uint64_t>(value) : fallback;
        }
        if (it->is_string()) {
            const auto text = trim_copy(it->get<std::string>());
            if (text.empty() || lower_copy(text) == "none") return fallback;
            const auto value = std::stod(text);
            return value >= 0.0 ? static_cast<uint64_t>(value) : fallback;
        }
    }
    catch (...) {
    }
    return fallback;
}

uint64_t json_u64_value(const Json & value, uint64_t fallback = 0) {
    try {
        if (value.is_number_unsigned()) return value.get<uint64_t>();
        if (value.is_number_integer()) {
            const auto number = value.get<int64_t>();
            return number >= 0 ? static_cast<uint64_t>(number) : fallback;
        }
        if (value.is_number_float()) {
            const auto number = value.get<double>();
            return number >= 0.0 ? static_cast<uint64_t>(number) : fallback;
        }
        if (value.is_string()) {
            const auto text = trim_copy(value.get<std::string>());
            if (text.empty() || lower_copy(text) == "none") return fallback;
            const auto number = std::stod(text);
            return number >= 0.0 ? static_cast<uint64_t>(number) : fallback;
        }
    }
    catch (...) {
    }
    return fallback;
}

std::vector<std::string> json_strings(const Json & value) {
    std::vector<std::string> output;
    auto append = [&](const Json & item) {
        auto text = item.is_string() ? item.get<std::string>()
                                     : json_string(
                                           Json{
                                               { "value", item }
        },
                                           "value");
        text = trim_copy(text);
        if (!text.empty()) output.push_back(std::move(text));
    };
    if (value.is_array()) std::ranges::for_each(value, append);
    else if (!value.is_null()) append(value);
    return output;
}

std::vector<uint64_t> json_u64_values(const Json & value) {
    std::vector<uint64_t> output;
    if (value.is_array()) {
        output.reserve(value.size());
        std::ranges::transform(value, std::back_inserter(output), [](const Json & item) { return json_u64_value(item); });
    }
    else if (!value.is_null()) { output.push_back(json_u64_value(value)); }
    return output;
}

HiCacheTokenSpan token_span_from_json(const Json & value) {
    HiCacheTokenSpan span;
    if (!value.is_object()) return span;
    span.path_id = json_string(value, "path_id");
    if (span.path_id.empty()) span.path_id = json_string(value, "token_path_id");
    span.begin = json_u64(value, "begin");
    span.end = json_u64(value, "end");
    span.token_count = json_u64(value, "token_count", span.end >= span.begin ? span.end - span.begin : 0);
    span.hash_algo = json_string(value, "hash_algo");
    span.valid = !span.path_id.empty() && span.end >= span.begin;
    return span;
}

struct FactMetadata {
    std::string fact_class;
    std::string role;
    std::vector<std::string> consumers;
};

/**
 * @brief 解析 catalog 写入的 fact metadata。
 *
 * C++ 侧只接受当前 schema 中的 `class`、`role`、`consumers` 三元组。
 * 不满足当前合同的 trace 会在普通 schema gate 中失败。
 */
FactMetadata fact_metadata_from_event(const TraceEvent & event) {
    FactMetadata metadata;
    if (!event.has_arg("fact")) throw std::invalid_argument("HiCache trace event args must contain fact object");
    const auto value = parse_json_arg(event.arg("fact"));
    if (!value.is_object()) throw std::invalid_argument("HiCache trace event fact must be an object");
    metadata.fact_class = json_string(value, "class");
    metadata.role = json_string(value, "role");
    const auto consumers = value.find("consumers");
    if (consumers != value.end()) metadata.consumers = json_strings(*consumers);
    if (metadata.fact_class.empty()) throw std::invalid_argument("HiCache trace event fact.class must be non-empty");
    if (metadata.role.empty()) throw std::invalid_argument("HiCache trace event fact.role must be non-empty");
    if (metadata.consumers.empty()) throw std::invalid_argument("HiCache trace event fact.consumers must be non-empty");
    return metadata;
}

bool consumer_list_contains(const std::vector<std::string> & consumers, std::string_view expected) {
    return std::ranges::any_of(consumers, [&](const auto & consumer) { return std::string_view{ consumer } == expected; });
}

/**
 * @brief 判断 fact 是否属于 state model 可消费的 path 事实。
 *
 * token dictionary 只能由这些事实水合。source_actual、oracle 或 diagnostics 即使携带
 * dictionary，也不能补齐 target model 输入，否则 cross-config prediction 会把 source
 * run 的观测结果当成 target 身份源。
 */
bool state_model_path_fact(const FactMetadata & metadata) {
    if (!consumer_list_contains(metadata.consumers, "hicache_state_model")) return false;
    if (metadata.fact_class != "workload_identity") return false;
    return metadata.role == "prefetch_candidate_anchor" || metadata.role == "cache_lookup_input" || metadata.role == "cache_extend_input"
           || metadata.role == "cache_lifecycle_commit";
}

bool state_model_dictionary_source(const TraceEvent & event) {
    const auto metadata = fact_metadata_from_event(event);
    if (!state_model_dictionary_phase(event, metadata.fact_class, metadata.role)) return false;
    return state_model_path_fact(metadata);
}

std::vector<std::string> event_string_list(const TraceEvent & event, std::initializer_list<std::string_view> keys) {
    for (const auto key : keys) {
        if (!event.has_arg(std::string{ key })) continue;
        const auto raw = event.arg(std::string{ key });
        auto values = json_strings(parse_json_arg(raw));
        if (values.empty()) {
            auto text = trim_copy(raw);
            if (!text.empty()) values.push_back(std::move(text));
        }
        if (!values.empty()) return values;
    }
    return {};
}

HiCacheToken token_from_json(const Json & value) {
    HiCacheToken token;
    auto append = [&](const Json & item) {
        try {
            if (item.is_number_unsigned()) token.words.push_back(static_cast<uint32_t>(item.get<uint64_t>()));
            else if (item.is_number_integer()) {
                const auto number = item.get<int64_t>();
                if (number >= 0) token.words.push_back(static_cast<uint32_t>(number));
            }
            else if (item.is_string()) {
                const auto number = std::stoll(trim_copy(item.get<std::string>()));
                if (number >= 0) token.words.push_back(static_cast<uint32_t>(number));
            }
        }
        catch (...) {
        }
    };

    if (value.is_array()) std::ranges::for_each(value, append);
    else append(value);
    return token;
}

HiCacheTokenPath token_path_from_json(const Json & value) {
    HiCacheTokenPath path;
    if (!value.is_array()) return path;
    std::ranges::for_each(value, [&](const Json & item) {
        auto token = token_from_json(item);
        if (!token.words.empty()) path.push_back(std::move(token));
    });
    return path;
}

HiCacheTokenPath slice_path(const HiCacheTokenPath & tokens, uint64_t begin, uint64_t end) {
    if (begin >= end || begin >= tokens.size()) return {};
    end = std::min<uint64_t>(end, tokens.size());
    HiCacheTokenPath result;
    result.reserve(static_cast<size_t>(end - begin));
    auto view = tokens | std::views::drop(static_cast<std::ranges::range_difference_t<HiCacheTokenPath>>(begin))
                | std::views::take(static_cast<std::ranges::range_difference_t<HiCacheTokenPath>>(end - begin));
    std::ranges::copy(view, std::back_inserter(result));
    return result;
}

} // namespace fact_detail

using fact_detail::consumer_list_contains;
using fact_detail::event_string_list;
using fact_detail::fact_metadata_from_event;
using fact_detail::json_string;
using fact_detail::json_u64;
using fact_detail::json_u64_values;
using fact_detail::lower_copy;
using fact_detail::parse_json_arg;
using fact_detail::slice_path;
using fact_detail::state_model_dictionary_source;
using fact_detail::token_path_from_json;
using fact_detail::token_span_from_json;

bool HiCacheFactParser::is_hicache_event(const TraceEvent & event) const {
    const auto target = lower_copy(event.arg("target_id"));
    if (target.starts_with("hiradix.") || target.starts_with("hicache.") || target.starts_with("hicache_controller.")) return true;
    if (event.cat == "hicache") return true;
    const auto domain = lower_copy(event.arg("domain"));
    if (domain == "python_probe" && lower_copy(event.name).contains("hicache")) return true;
    return event.name.starts_with("HiCache::") || event.name.starts_with("hicache_");
}

void HiCacheFactParser::observe_token_dictionaries(const TraceEvent & event) {
    if (!is_hicache_event(event)) return;
    if (!state_model_dictionary_source(event)) return;
    /**
     * @brief dictionary 是 span-only fact 的同合同 side table。
     *
     * 只从 completed state-model path fact 中观察，避免 start phase 的半成品或诊断事实污染模型。
     */
    std::ranges::for_each(event.args_map(), [&](const auto & item) {
        const auto & [key, value] = item;
        if (key.contains("dictionary")) observe_dictionary_value(value);
    });
}

void HiCacheFactParser::observe_dictionary_value(const std::string & raw) {
    const auto value = parse_json_arg(raw);
    if (value.is_array()) {
        for (const auto & item : value) observe_dictionary_value(item.dump());
        return;
    }
    if (!value.is_object()) return;
    auto path_id = json_string(value, "token_path_id");
    if (path_id.empty()) path_id = json_string(value, "path_id");
    if (path_id.empty()) return;
    const auto ids = value.find("token_ids");
    if (ids == value.end()) return;
    auto tokens = token_path_from_json(*ids);
    if (!tokens.empty()) token_paths_[path_id] = std::move(tokens);
}

HiCacheTokenSpan HiCacheFactParser::parse_span(const TraceEvent & event, const std::string & key) const {
    /**
     * @brief span 只记录 token path 的半开区间。
     *
     * page identity 必须由 target config 重新投影，不能直接沿用 source trace 中的 page id。
     */
    HiCacheTokenSpan span;
    const auto value = parse_json_arg(event.arg(key));
    return token_span_from_json(value);
}

HiCacheTokenPath HiCacheFactParser::resolve_span(const HiCacheTokenSpan & span) const {
    /**
     * @brief 解析失败时返回空 path，让 router/quality 报合同缺口。
     *
     * 这里不从其他 role 或 source actual 中寻找替代 path。
     */
    if (!span.valid) return {};
    const auto it = token_paths_.find(span.path_id);
    if (it == token_paths_.end()) return {};
    return slice_path(it->second, span.begin, span.end);
}

bool hicache_fact_has_resolved_full_path(const HiCacheFact & fact) {
    if (!fact.full_path_span.valid) return false;
    if (fact.full_path_span.token_count == 0) return fact.full_path_span.begin == fact.full_path_span.end;
    return static_cast<uint64_t>(fact.full_path_tokens.size()) == fact.full_path_span.token_count;
}

bool HiCacheFact::has_consumer(const std::string & consumer) const { return consumer_list_contains(consumers, consumer); }

std::vector<HiCacheBatchPathEntry> HiCacheFactParser::parse_batch_paths(const TraceEvent & event) const {
    const auto request_ids_value = parse_json_arg(event.arg("request_ids"));
    const auto request_ids = request_ids_value.is_array() ? fact_detail::json_strings(request_ids_value) : std::vector<std::string>{};
    const auto positions = parse_json_arg(event.arg("request_positions"));
    const auto spans = parse_json_arg(event.arg("full_path_spans"));
    const auto token_counts_value = parse_json_arg(event.arg("token_counts"));
    const auto token_counts = token_counts_value.is_array() ? json_u64_values(token_counts_value) : std::vector<uint64_t>{};

    std::vector<HiCacheBatchPathEntry> entries;
    entries.reserve(request_ids.size());
    for (size_t index = 0; index < request_ids.size(); ++index) {
        HiCacheBatchPathEntry entry;
        entry.request_id = request_ids[index];
        entry.position = index;
        if (positions.is_array() && index < positions.size() && positions[index].is_object()) entry.position = json_u64(positions[index], "index", index);
        if (spans.is_array() && index < spans.size()) {
            entry.full_path_span = token_span_from_json(spans[index]);
            entry.full_path_tokens = resolve_span(entry.full_path_span);
        }
        entry.token_count = index < token_counts.size() ? token_counts[index] : entry.full_path_span.token_count;
        entries.push_back(std::move(entry));
    }
    return entries;
}

void parse_batch_contract_fields(HiCacheFact & fact, const TraceEvent & event) {
    const auto request_ids = parse_json_arg(event.arg("request_ids"));
    const auto positions = parse_json_arg(event.arg("request_positions"));
    const auto dictionaries = parse_json_arg(event.arg("token_dictionaries"));
    const auto spans = parse_json_arg(event.arg("full_path_spans"));
    const auto token_counts = parse_json_arg(event.arg("token_counts"));

    fact.batch_request_ids_array = request_ids.is_array();
    fact.batch_positions_array = positions.is_array();
    fact.batch_token_dictionaries_array = dictionaries.is_array();
    fact.batch_spans_array = spans.is_array();
    fact.batch_token_counts_array = token_counts.is_array();
    fact.batch_request_id_count = request_ids.is_array() ? static_cast<uint64_t>(request_ids.size()) : 0;
    fact.batch_position_count = positions.is_array() ? static_cast<uint64_t>(positions.size()) : 0;
    fact.batch_token_dictionary_count = dictionaries.is_array() ? static_cast<uint64_t>(dictionaries.size()) : 0;
    fact.batch_span_count = spans.is_array() ? static_cast<uint64_t>(spans.size()) : 0;
    fact.batch_token_count_count = token_counts.is_array() ? static_cast<uint64_t>(token_counts.size()) : 0;

    std::unordered_set<std::string> request_ids_seen;
    if (request_ids.is_array()) {
        for (const auto & item : request_ids) {
            const auto text = item.is_string() ? item.get<std::string>()
                                               : json_string(
                                                     nlohmann::json{
                                                         { "value", item }
            },
                                                     "value");
            if (!text.empty()) request_ids_seen.insert(text);
        }
    }
    fact.batch_request_ids_unique = request_ids.is_array() && request_ids_seen.size() == request_ids.size();

    if (!positions.is_array() || !request_ids.is_array() || positions.size() != request_ids.size()) {
        fact.batch_positions_cover_indexes = false;
        fact.batch_positions_match_request_ids = positions.is_array() && request_ids.is_array();
        return;
    }
    std::unordered_set<uint64_t> indexes_seen;
    bool request_ids_match = true;
    for (size_t index = 0; index < positions.size(); ++index) {
        if (!positions[index].is_object()) {
            request_ids_match = false;
            continue;
        }
        const auto position_index = json_u64(positions[index], "index", request_ids.size());
        indexes_seen.insert(position_index);
        const auto position_request_id = json_string(positions[index], "request_id");
        const auto expected_request_id = request_ids[index].is_string() ? request_ids[index].get<std::string>()
                                                                        : json_string(
                                                                              nlohmann::json{
                                                                                  { "value", request_ids[index] }
        },
                                                                              "value");
        if (!position_request_id.empty() && position_request_id != expected_request_id) request_ids_match = false;
    }
    fact.batch_positions_cover_indexes = indexes_seen.size() == request_ids.size();
    for (size_t index = 0; index < request_ids.size() && fact.batch_positions_cover_indexes; ++index) {
        if (!indexes_seen.contains(index)) fact.batch_positions_cover_indexes = false;
    }
    fact.batch_positions_match_request_ids = request_ids_match;
}

HiCacheFact HiCacheFactParser::parse(size_t node_id, const TraceEvent & event) const {
    /**
     * @brief parse 只做字段规范化和 token span 水合。
     *
     * fact 是否能驱动状态机，由 route_hicache_fact() 根据 consumer、class/role 和
     * phase 再做硬门禁。
     */
    HiCacheFact fact;
    const auto metadata = fact_metadata_from_event(event);
    fact.source_node_id = node_id;
    fact.source_event_index = event.index;
    fact.ts = event.ts;
    fact.dur = event.dur;
    fact.event_name = event.name;
    fact.target_id = event.arg("target_id");
    fact.fact_class = metadata.fact_class;
    fact.role = metadata.role;
    fact.consumers = metadata.consumers;
    fact.phase = event.arg("phase");
    fact.is_start = fact.phase == "start";
    fact.is_end = fact.phase == "end";
    fact.request_id = event.arg("request_id");
    fact.operation_id = event.arg("operation_id", event.arg("node_id"));
    fact.cache_scope = event.arg("cache_scope");
    fact.lifecycle_kind = event.arg("lifecycle_kind");
    fact.storage_source = event.arg("storage_source", event.arg("readable_source", "storage_backend_readable"));
    fact.batch_kind = event.arg("batch_kind");
    fact.seq_no = event.arg_u64("seq_no", 0);
    fact.source_page_size = event.arg_u64("source_page_size", 0);
    fact.token_count = event.arg_u64("token_count", 0);
    fact.batch_size = event.arg_u64("batch_size", 0);
    fact.priority = static_cast<int64_t>(event.arg_u64("priority", 0));
    fact.full_path_span = parse_span(event, "full_path_span");
    fact.full_path_tokens = resolve_span(fact.full_path_span);
    if (fact.role == "cache_extend_input") {
        parse_batch_contract_fields(fact, event);
        fact.batch_paths = parse_batch_paths(event);
    }
    fact.storage_page_hashes = event_string_list(event, { "page_hashes", "hash_pages", "storage_hashes", "storage_keys", "page_keys", "hit_hash_pages" });
    return fact;
}

} // namespace markov::trace_graph::modules::hicache
