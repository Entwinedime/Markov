/**
 * @file
 * @brief HiCache trace fact parser 与 token dictionary 解析。
 */
#include "trace_graph/modules/hicache/hicache_fact.hpp"

#include "trace_graph/core/trace_event.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>

namespace TraceGraph {

namespace {

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

bool bool_value(const TraceEvent & event, const std::string & key, bool fallback) {
    if (!event.has_arg(key)) return fallback;
    const auto value = lower_copy(trim_copy(event.arg(key)));
    if (value == "true" || value == "1" || value == "yes" || value == "on") return true;
    if (value == "false" || value == "0" || value == "no" || value == "off") return false;
    return fallback;
}

bool completed_event(const TraceEvent & event) {
    const auto phase = lower_copy(trim_copy(event.arg("phase")));
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

struct FactMetadata {
    std::string fact_class;
    std::string role;
    std::vector<std::string> consumers;
};

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

bool state_model_path_fact(const FactMetadata & metadata) {
    if (!consumer_list_contains(metadata.consumers, "hicache_state_model")) return false;
    if (metadata.fact_class == "target_policy_input") return metadata.role == "prefetch_decision";
    if (metadata.fact_class != "workload_identity") return false;
    return metadata.role == "request_bound_match_anchor" || metadata.role == "request_lifecycle_anchor" || metadata.role == "request_admission";
}

bool state_model_dictionary_source(const TraceEvent & event) {
    if (!completed_event(event)) return false;
    return state_model_path_fact(fact_metadata_from_event(event));
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

} // namespace

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
    std::ranges::for_each(event.args, [&](const auto & item) {
        const auto & [key, value] = item;
        if (key.contains("dictionary")) observe_dictionary_value(value);
    });
}

void HiCacheFactParser::observe_dictionary_value(const std::string & raw) {
    const auto value = parse_json_arg(raw);
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
    HiCacheTokenSpan span;
    const auto value = parse_json_arg(event.arg(key));
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

HiCacheTokenPath HiCacheFactParser::resolve_span(const HiCacheTokenSpan & span) const {
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

bool HiCacheFact::has_consumer(const std::string & consumer) const {
    return consumer_list_contains(consumers, consumer);
}

HiCacheFact HiCacheFactParser::parse(size_t node_id, const TraceEvent & event) const {
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
    fact.check_kind = event.arg("check_kind");
    fact.lifecycle_kind = event.arg("lifecycle_kind");
    fact.admission_kind = event.arg("admission_kind");
    fact.storage_source = event.arg("storage_source", event.arg("readable_source", "storage_backend_readable"));
    fact.seq_no = event.arg_u64("seq_no", 0);
    fact.source_page_size = event.arg_u64("source_page_size", 0);
    fact.token_count = event.arg_u64("token_count", 0);
    fact.max_new_tokens = event.arg_u64("max_new_tokens", 0);
    fact.truncation_align_size = event.arg_u64("truncation_align_size", 0);
    fact.priority = static_cast<int64_t>(event.arg_u64("priority", 0));
    fact.has_chunked_req = bool_value(event, "has_chunked_req", false);
    fact.ignore_eos = bool_value(event, "ignore_eos", false);
    fact.full_path_span = parse_span(event, "full_path_span");
    fact.full_path_tokens = resolve_span(fact.full_path_span);
    fact.storage_page_hashes = event_string_list(event, { "page_hashes", "hash_pages", "storage_hashes", "storage_keys", "page_keys", "hit_hash_pages" });
    return fact;
}

} // namespace TraceGraph
