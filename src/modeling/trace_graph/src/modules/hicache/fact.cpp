/**
 * @file
 * @brief HiCache trace-fact and token-dictionary parser.
 */
#include "markov/trace_graph/modules/hicache/fact.hpp"

#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/core/trace_event.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <limits>
#include <optional>
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
    catch (const Json::exception &) {
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

uint64_t json_u64_value(const Json & value, uint64_t fallback = 0);

uint64_t json_u64(const Json & object, const std::string & key, uint64_t fallback = 0) {
    if (!object.is_object()) return fallback;
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) return fallback;
    return json_u64_value(*it, fallback);
}

uint64_t json_u64_value(const Json & value, uint64_t fallback) {
    if (value.is_null()) return fallback;
    if (value.is_number_unsigned()) return value.get<uint64_t>();
    if (value.is_number_integer()) {
        const auto number = value.get<int64_t>();
        if (number >= 0) return static_cast<uint64_t>(number);
    }
    else if (value.is_number_float()) {
        if (const auto number = core::truncate_to_u64(value.get<double>())) return *number;
    }
    else if (value.is_string()) {
        const auto text = trim_copy(value.get<std::string>());
        if (text.empty() || lower_copy(text) == "none") return fallback;
        if (const auto number = core::parse_u64(text)) return *number;
    }
    throw std::invalid_argument("HiCache fact field must be a non-negative uint64-compatible value");
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

using FactMetadata = HiCacheFactMetadata;

/**
 * @brief Parses the fact metadata written by the probe catalog.
 *
 * The C++ boundary requires non-empty `class`, `role`, and `consumers`; malformed metadata
 * fails before routing rather than being reinterpreted as another fact class.
 */
FactMetadata fact_metadata_from_event(const TraceEvent & event) {
    FactMetadata metadata;
    const auto raw_fact = event.arg("fact");
    if (raw_fact.empty()) throw std::invalid_argument("HiCache trace event args must contain fact object");
    const auto value = parse_json_arg(raw_fact);
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
 * @brief Tests whether a fact may hydrate a state-model token path.
 *
 * Source-actual, oracle, and diagnostics dictionaries are excluded even when present. Using
 * them would turn a source-run outcome into target identity during cross-config prediction.
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

HiCacheToken token_from_json(const Json & value) {
    HiCacheToken token;
    auto append = [&](const Json & item) {
        std::optional<uint64_t> number;
        if (item.is_number_unsigned()) number = item.get<uint64_t>();
        else if (item.is_number_integer()) {
            const auto signed_number = item.get<int64_t>();
            if (signed_number >= 0) number = static_cast<uint64_t>(signed_number);
        }
        else if (item.is_string()) number = core::parse_exact_u64(item.get_ref<const std::string &>());
        if (!number || *number > std::numeric_limits<uint32_t>::max()) { throw std::invalid_argument("HiCache token word must be an unsigned 32-bit integer"); }
        token.words.push_back(static_cast<uint32_t>(*number));
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

std::string json_item_text(const Json & item) {
    if (item.is_string()) return item.get<std::string>();
    return json_string(
        Json{
            { "value", item }
    },
        "value");
}

bool batch_request_ids_unique(const Json & request_ids) {
    if (!request_ids.is_array()) return false;
    std::unordered_set<std::string> seen;
    for (const auto & item : request_ids) {
        const auto text = json_item_text(item);
        if (!text.empty()) seen.insert(text);
    }
    return seen.size() == request_ids.size();
}

struct BatchPositionValidation {
    bool covers_indexes = false;
    bool matches_request_ids = false;
};

BatchPositionValidation validate_batch_positions(const Json & positions, const Json & request_ids) {
    if (!positions.is_array() || !request_ids.is_array() || positions.size() != request_ids.size()) {
        return BatchPositionValidation{
            .covers_indexes = false,
            .matches_request_ids = positions.is_array() && request_ids.is_array(),
        };
    }

    std::unordered_set<uint64_t> seen_indexes;
    bool request_ids_match = true;
    for (size_t index = 0; index < positions.size(); ++index) {
        if (!positions[index].is_object()) {
            request_ids_match = false;
            continue;
        }
        seen_indexes.insert(json_u64(positions[index], "index", request_ids.size()));
        const auto position_request_id = json_string(positions[index], "request_id");
        const auto expected_request_id = json_item_text(request_ids[index]);
        if (!position_request_id.empty() && position_request_id != expected_request_id) request_ids_match = false;
    }

    bool covers_indexes = seen_indexes.size() == request_ids.size();
    for (size_t index = 0; index < request_ids.size() && covers_indexes; ++index) {
        if (!seen_indexes.contains(index)) covers_indexes = false;
    }
    return BatchPositionValidation{
        .covers_indexes = covers_indexes,
        .matches_request_ids = request_ids_match,
    };
}

} // namespace fact_detail

using fact_detail::batch_request_ids_unique;
using fact_detail::consumer_list_contains;
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
using fact_detail::validate_batch_positions;

HiCacheFactMetadata parse_hicache_fact_metadata(const TraceEvent & event) { return fact_metadata_from_event(event); }

uint64_t hicache_fact_boundary_timestamp(const TraceEvent & event) {
    if (event.arg("phase") != "end") return event.ts;
    return core::checked_add_u64(event.ts, event.dur, "HiCache end-fact timestamp exceeds uint64 range");
}

HiCacheTokenSpan parse_hicache_token_span_arg(const TraceEvent & event, std::string_view key) { return token_span_from_json(parse_json_arg(event.arg(key))); }

bool HiCacheFactParser::is_hicache_event(const TraceEvent & event) const {
    if (event.source_channel != core::TraceSourceChannel::PythonProbe) return false;
    if (event.cat == "hicache" || event.name.starts_with("HiCache::") || event.name.starts_with("hicache_")) return true;

    const auto raw = event.args_json_view();
    if (raw.find("target_id") != std::string_view::npos || event.has_arg_override("target_id")) {
        const auto target = lower_copy(event.arg("target_id"));
        if (target.starts_with("hiradix.") || target.starts_with("hicache.") || target.starts_with("hicache_controller.")) return true;
    }
    if (raw.find("domain") == std::string_view::npos && !event.has_arg_override("domain")) return false;
    const auto domain = lower_copy(event.arg("domain"));
    return domain == "python_probe" && lower_copy(event.name).contains("hicache");
}

void HiCacheFactParser::observe_token_dictionaries(const TraceEvent & event) {
    if (!state_model_dictionary_source(event)) return;
    // A dictionary is a same-contract side table for span-only facts. Only eligible phases
    // may populate it, preventing partial or diagnostic events from contaminating replay.
    const auto token_dictionary = event.arg("token_dictionary");
    if (!token_dictionary.empty()) observe_dictionary_value(token_dictionary);
    const auto token_dictionaries = event.arg("token_dictionaries");
    if (!token_dictionaries.empty()) observe_dictionary_value(token_dictionaries);
}

void HiCacheFactParser::observe_dictionary_value(const std::string & raw) {
    const auto value = parse_json_arg(raw);
    auto observe = [&](auto && self, const fact_detail::Json & item) -> void {
        if (item.is_array()) {
            for (const auto & child : item) self(self, child);
            return;
        }
        if (!item.is_object()) return;
        auto path_id = json_string(item, "token_path_id");
        if (path_id.empty()) path_id = json_string(item, "path_id");
        if (path_id.empty()) return;
        const auto ids = item.find("token_ids");
        if (ids == item.end()) return;
        auto tokens = token_path_from_json(*ids);
        if (tokens.empty()) return;
        const auto existing = token_paths_.find(path_id);
        if (existing != token_paths_.end() && existing->second != tokens) {
            throw std::invalid_argument("HiCache token dictionary path_id maps to conflicting token sequences: " + path_id);
        }
        token_paths_.insert_or_assign(std::move(path_id), std::move(tokens));
    };
    observe(observe, value);
}

HiCacheTokenSpan HiCacheFactParser::parse_span(const TraceEvent & event, std::string_view key) const {
    // Spans identify token intervals only; target page identity is always reprojected.
    HiCacheTokenSpan span;
    const auto value = parse_json_arg(event.arg(key));
    return token_span_from_json(value);
}

HiCacheTokenPath HiCacheFactParser::resolve_span(const HiCacheTokenSpan & span) const {
    // Missing dictionary entries remain empty so quality gates can report the contract gap.
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

bool HiCacheFact::has_consumer(std::string_view consumer) const { return consumer_list_contains(consumers, consumer); }

void HiCacheFactParser::parse_batch_fields(HiCacheFact & fact, const TraceEvent & event) const {
    const auto request_ids_value = parse_json_arg(event.arg("request_ids"));
    const auto request_ids = request_ids_value.is_array() ? fact_detail::json_strings(request_ids_value) : std::vector<std::string>{};
    const auto positions = parse_json_arg(event.arg("request_positions"));
    const auto dictionaries = parse_json_arg(event.arg("token_dictionaries"));
    const auto spans = parse_json_arg(event.arg("full_path_spans"));
    const auto token_counts_value = parse_json_arg(event.arg("token_counts"));
    const auto token_counts = token_counts_value.is_array() ? json_u64_values(token_counts_value) : std::vector<uint64_t>{};

    fact.batch_request_ids_array = request_ids_value.is_array();
    fact.batch_positions_array = positions.is_array();
    fact.batch_token_dictionaries_array = dictionaries.is_array();
    fact.batch_spans_array = spans.is_array();
    fact.batch_token_counts_array = token_counts_value.is_array();
    fact.batch_request_id_count = request_ids_value.is_array() ? static_cast<uint64_t>(request_ids_value.size()) : 0;
    fact.batch_position_count = positions.is_array() ? static_cast<uint64_t>(positions.size()) : 0;
    fact.batch_token_dictionary_count = dictionaries.is_array() ? static_cast<uint64_t>(dictionaries.size()) : 0;
    fact.batch_span_count = spans.is_array() ? static_cast<uint64_t>(spans.size()) : 0;
    fact.batch_token_count_count = token_counts_value.is_array() ? static_cast<uint64_t>(token_counts_value.size()) : 0;

    fact.batch_paths.reserve(request_ids.size());
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
        fact.batch_paths.push_back(std::move(entry));
    }

    fact.batch_request_ids_unique = batch_request_ids_unique(request_ids_value);
    const auto position_validation = validate_batch_positions(positions, request_ids_value);
    fact.batch_positions_cover_indexes = position_validation.covers_indexes;
    fact.batch_positions_match_request_ids = position_validation.matches_request_ids;
}

HiCacheFact HiCacheFactParser::parse(size_t node_id, const TraceEvent & event) const {
    // Parsing normalizes fields and hydrates spans only. Routing remains a separate strict gate.
    HiCacheFact fact;
    const auto metadata = fact_metadata_from_event(event);
    fact.source_node_id = node_id;
    fact.source_event_index = event.index;
    fact.ts = hicache_fact_boundary_timestamp(event);
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
    fact.operation_id = event.arg("operation_id");
    if (fact.operation_id.empty()) fact.operation_id = event.arg("node_id");
    fact.cache_scope = event.arg("cache_scope");
    fact.lifecycle_kind = event.arg("lifecycle_kind");
    fact.batch_kind = event.arg("batch_kind");
    fact.seq_no = event.arg_u64("seq_no", 0);
    fact.source_page_size = event.arg_u64("source_page_size", 0);
    fact.token_count = event.arg_u64("token_count", 0);
    fact.batch_size = event.arg_u64("batch_size", 0);
    fact.priority = core::parse_i64(event.arg("priority")).value_or(0);
    fact.full_path_span = parse_span(event, "full_path_span");
    fact.full_path_tokens = resolve_span(fact.full_path_span);
    if (fact.role == "cache_extend_input") parse_batch_fields(fact, event);
    return fact;
}

} // namespace markov::trace_graph::modules::hicache
