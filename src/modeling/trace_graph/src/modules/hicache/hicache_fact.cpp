#include "trace_graph/modules/hicache/hicache_fact.hpp"

#include "trace_graph/core/trace_event.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace TraceGraph {

namespace {

using Json = nlohmann::json;

bool starts_with(const std::string & text, const std::string & prefix) { return text.rfind(prefix, 0) == 0; }

bool ends_with(const std::string & text, const std::string & suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool contains(const std::string & text, const std::string & needle) { return text.find(needle) != std::string::npos; }

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

std::string trim(const std::string & value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

std::string arg_string(const TraceEvent & event, const std::string & key) {
    auto it = event.args.find(key);
    return it == event.args.end() ? "" : it->second;
}

std::string normalize_tier(const std::string & value) {
    auto tier = upper(trim(value));
    if (tier == "DEVICE" || tier == "GPU" || tier == "L1") return "L1";
    if (tier == "HOST" || tier == "CPU" || tier == "CPU_PINNED" || tier == "L2") return "L2";
    if (tier == "STORAGE" || tier == "DISK" || tier == "L3") return "L3";
    return "";
}

bool bool_arg(const TraceEvent & event, const std::string & key, bool fallback) {
    auto value = lower(trim(event.arg(key)));
    if (value.empty()) return fallback;
    if (value == "true" || value == "1" || value == "yes") return true;
    if (value == "false" || value == "0" || value == "no") return false;
    return fallback;
}

std::string unescape_json_quote_markers(const std::string & value) {
    // TraceScanner 为了性能不完整反转义 JSON string。HiCache page_identity 经常以
    // list 字符串出现，这里只处理会污染 page key 的最小转义。
    std::string result;
    result.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size() && (value[i + 1] == '"' || value[i + 1] == '\\')) {
            result.push_back(value[i + 1]);
            ++i;
        }
        else {
            result.push_back(value[i]);
        }
    }
    return result;
}

std::vector<std::string> pages_from_json_array(const std::string & raw) {
    std::vector<std::string> pages;
    try {
        auto value = Json::parse(raw);
        if (value.is_array()) {
            for (const auto & item : value) {
                if (item.is_null()) continue;
                if (item.is_string())
                    pages.push_back(trim(item.get<std::string>()));
                else if (item.is_number_unsigned())
                    pages.push_back(std::to_string(item.get<uint64_t>()));
                else if (item.is_number_integer())
                    pages.push_back(std::to_string(item.get<int64_t>()));
                else
                    pages.push_back(item.dump());
            }
        }
        else if (value.is_string())
            pages.push_back(trim(value.get<std::string>()));
    }
    catch (...) {
    }
    return pages;
}

std::vector<std::string> pages_from_json_value(const Json & value) {
    std::vector<std::string> pages;
    if (value.is_null()) return pages;
    if (value.is_array()) {
        for (const auto & item : value) {
            if (item.is_null()) continue;
            if (item.is_string())
                pages.push_back(trim(item.get<std::string>()));
            else if (item.is_number_unsigned())
                pages.push_back(std::to_string(item.get<uint64_t>()));
            else if (item.is_number_integer())
                pages.push_back(std::to_string(item.get<int64_t>()));
            else
                pages.push_back(item.dump());
        }
    }
    else if (value.is_string()) {
        auto raw = trim(value.get<std::string>());
        auto nested = pages_from_json_array(raw);
        if (!nested.empty())
            pages = std::move(nested);
        else if (!raw.empty())
            pages.push_back(raw);
    }
    else if (value.is_number_unsigned())
        pages.push_back(std::to_string(value.get<uint64_t>()));
    else if (value.is_number_integer())
        pages.push_back(std::to_string(value.get<int64_t>()));
    return pages;
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

bool json_bool_value(const Json & object, const std::string & key, bool fallback, bool * present = nullptr) {
    if (present) *present = false;
    if (!object.is_object()) return fallback;
    auto it = object.find(key);
    if (it == object.end() || it->is_null()) return fallback;
    if (present) *present = true;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0.0;
    if (it->is_string()) {
        auto text = lower(trim(it->get<std::string>()));
        if (text == "true" || text == "1" || text == "yes") return true;
        if (text == "false" || text == "0" || text == "no") return false;
        if (present) *present = false;
    }
    return fallback;
}

uint64_t ready_page_count_from_progress(const Json & progress, uint64_t fallback_page_size) {
    const uint64_t page_size = json_u64_value(progress, "page_size", fallback_page_size);
    const uint64_t completed_tokens = json_u64_value(progress, "completed_tokens", 0);
    if (page_size > 0 && completed_tokens > 0) return completed_tokens / page_size;

    const uint64_t ready_pages = json_u64_value(progress, "ready_pages_estimate", 0);
    if (ready_pages > 0) return ready_pages;

    const uint64_t loaded_tokens = json_u64_value(progress, "loaded_tokens_evidence", 0);
    if (page_size > 0 && loaded_tokens > 0) return loaded_tokens / page_size;
    return 0;
}

} // namespace

bool HiCacheFactParser::is_hicache_event(const TraceEvent & event) const {
    // 真实执行的 HiCache/Python probe 事件都应被 state model 看见；非执行
    // state_snapshot 已经在 ChromeTraceIO 被过滤，不会走到这里。
    if (event.cat == "hicache") return true;
    if (starts_with(event.name, "HiCache::")) return true;
    if (starts_with(event.name, "hicache_")) return true;
    auto domain = arg_string(event, "domain");
    return domain == "hicache" || (domain == "python_probe" && contains(event.name, "hicache"));
}

HiCacheFact HiCacheFactParser::parse(size_t node_id, const TraceEvent & event) const {
    HiCacheFact fact;
    fact.source_node_id = node_id;
    fact.source_event_index = event.index;
    fact.ts = event.ts;
    fact.event_name = event.name;
    fact.role = infer_role(event);
    if (fact.role.empty()) fact.role = "unknown";
    fact.request_id = event.arg("request_id");
    fact.operation_id = event.arg("operation_id", event.arg("node_id"));
    fact.cache_scope = event.pid.empty() ? event.arg("pid", "-1") : event.pid;
    fact.tier_src = normalize_tier(event.arg("tier_src"));
    fact.tier_dst = normalize_tier(event.arg("tier_dst"));
    fact.direction = lower(trim(event.arg("direction")));
    fact.page_size = event.arg_u64("page_size", 0);
    fact.prefix_len = event.arg_u64("prefix_len", 0);
    fact.new_input_tokens = event.arg_u64("new_input_tokens", 0);
    fact.completed_tokens = event.arg_u64("completed_tokens", 0);
    fact.requested_tokens = event.arg_u64("requested_tokens", 0);
    fact.evicted_tokens = event.arg_u64("evicted_tokens", 0);
    fact.pages = parse_page_identity(event);
    fact.source_pages = fact.pages;
    const bool insert_had_source_pages = fact.role == "insert" && !fact.pages.empty();
    fact.target_pages = parse_page_arg(event, "target_page_identity");
    fact.target_pages_by_page_size = parse_page_arg_by_page_size(event, "target_page_identity_page");
    fact.radix_removed_pages = parse_page_arg(event, "radix_removed_page_identity");
    fact.target_radix_removed_pages = parse_page_arg(event, "target_radix_removed_page_identity");
    fact.target_radix_removed_pages_by_page_size = parse_page_arg_by_page_size(event, "target_radix_removed_page_identity_page");
    parse_prefetch_progress(event, fact);
    if (fact.prefetch_ready_page_count == 0) {
        if (fact.page_size > 0 && fact.completed_tokens > 0) fact.prefetch_ready_page_count = fact.completed_tokens / fact.page_size;
    }
    if (fact.role == "insert" && fact.prefix_len > 0 && fact.page_size > 0 && !fact.pages.empty()) {
        const size_t prefix_pages = std::min(fact.pages.size(), static_cast<size_t>(fact.prefix_len / fact.page_size));
        fact.pages.erase(fact.pages.begin(), fact.pages.begin() + static_cast<long>(prefix_pages));
    }
    fact.is_start = ends_with(event.name, "_start");
    fact.requires_page_identity = role_requires_page_identity(fact.role, event);
    if (fact.role == "insert" && insert_had_source_pages && fact.pages.empty()) fact.requires_page_identity = false;
    // SGLang inc_lock_ref/dec_lock_ref 会沿父链更新到 root。root 本身没有 page
    // 身份，且 lock_delta=0 时只是 no-op 观测，不应破坏 page identity 严格契约。
    if ((fact.role == "lock_ref_inc" || fact.role == "lock_ref_dec") && fact.pages.empty() && event.arg_u64("lock_delta", 0) == 0)
        fact.requires_page_identity = false;
    fact.dirty = bool_arg(event, "dirty", true);
    fact.backuped = bool_arg(event, "backuped", false);
    fact.chunked = bool_arg(event, "chunked", false);
    return fact;
}

std::string HiCacheFactParser::infer_role(const TraceEvent & event) const {
    auto role = lower(trim(event.arg("event_role")));
    if (!role.empty()) return role;

    auto direction = lower(trim(event.arg("direction")));
    auto src = normalize_tier(event.arg("tier_src"));
    auto dst = normalize_tier(event.arg("tier_dst"));
    if (!src.empty() && !dst.empty() && !direction.empty()) return lower(src) + "_to_" + lower(dst) + "_" + direction;
    if (!direction.empty()) return direction;

    auto name = lower(event.name);
    if (starts_with(name, "hicache_")) name = name.substr(std::string("hicache_").size());
    if (ends_with(name, "_start")) name.resize(name.size() - std::string("_start").size());
    if (ends_with(name, "_end")) name.resize(name.size() - std::string("_end").size());
    if (contains(name, "l2_l1")) return "l2_to_l1";
    if (contains(name, "l1_l2")) return "l1_to_l2";
    if (contains(name, "l3_l2") || contains(name, "page_transfer")) return "l3_to_l2";
    if (contains(name, "l2_l3") || contains(name, "write_storage") || contains(name, "page_backup")) return "l2_to_l3";
    if (contains(name, "insert")) return "insert";
    if (contains(name, "load_back")) return "load_back";
    if (contains(name, "prefetch")) return "prefetch";
    if (contains(name, "write_backup")) return "write_backup";
    if (contains(name, "inc_lock_ref")) return "lock_ref_inc";
    if (contains(name, "dec_lock_ref")) return "lock_ref_dec";
    if (contains(name, "evict")) return "evict";
    if (contains(name, "lookup") || contains(name, "match_prefix")) return "lookup";
    return name.empty() ? "unknown" : name;
}

std::vector<std::string> HiCacheFactParser::parse_page_identity(const TraceEvent & event) const {
    auto pages = parse_page_arg(event, "page_identity");
    if (!pages.empty()) return pages;
    return parse_page_arg(event, "hash_value");
}

std::vector<std::string> HiCacheFactParser::parse_page_arg(const TraceEvent & event, const std::string & key) const {
    auto raw = unescape_json_quote_markers(trim(event.arg(key)));
    if (raw.empty()) return {};

    auto pages = pages_from_json_array(raw);
    if (pages.empty()) {
        std::string normalized = raw;
        for (char & ch : normalized) {
            if (ch == '[' || ch == ']' || ch == '"' || ch == '\'' || ch == '|' || ch == ',' || ch == ';' || std::isspace(static_cast<unsigned char>(ch)))
                ch = ' ';
        }
        std::istringstream iss(normalized);
        std::string token;
        while (iss >> token) pages.push_back(trim(token));
    }

    std::vector<std::string> deduped;
    std::unordered_set<std::string> seen;
    for (auto & page : pages) {
        page = trim(page);
        if (page.empty()) continue;
        if (seen.insert(page).second) deduped.push_back(page);
    }
    return deduped;
}

std::unordered_map<uint64_t, std::vector<std::string>> HiCacheFactParser::parse_page_arg_by_page_size(const TraceEvent & event,
                                                                                                      const std::string & prefix) const {
    std::unordered_map<uint64_t, std::vector<std::string>> result;
    for (const auto & [key, _] : event.args) {
        if (!starts_with(key, prefix)) continue;
        const auto suffix = key.substr(prefix.size());
        if (suffix.empty()) continue;
        uint64_t page_size = 0;
        bool valid = true;
        for (const char ch : suffix) {
            if (!std::isdigit(static_cast<unsigned char>(ch))) {
                valid = false;
                break;
            }
            page_size = page_size * 10 + static_cast<uint64_t>(ch - '0');
        }
        if (!valid || page_size == 0) continue;
        auto pages = parse_page_arg(event, key);
        if (!pages.empty()) result[page_size] = std::move(pages);
    }
    return result;
}

void HiCacheFactParser::parse_prefetch_progress(const TraceEvent & event, HiCacheFact & fact) const {
    if (fact.role != "prefetch_progress") return;

    if (event.has_arg("prefetch_done")) {
        const auto raw_done = lower(trim(event.arg("prefetch_done")));
        if (raw_done == "true" || raw_done == "1" || raw_done == "yes" || raw_done == "false" || raw_done == "0" || raw_done == "no") {
            fact.prefetch_check_available = true;
            fact.prefetch_check_return = raw_done == "true" || raw_done == "1" || raw_done == "yes";
        }
    }

    auto raw = unescape_json_quote_markers(trim(event.arg("prefetch_progress_state")));
    if (raw.empty() || lower(raw) == "none" || lower(raw) == "null") return;

    Json progress;
    try {
        progress = Json::parse(raw);
        if (progress.is_string()) progress = Json::parse(progress.get<std::string>());
    }
    catch (...) {
        return;
    }
    if (!progress.is_object()) return;

    fact.prefetch_progress_evidence = true;
    if (fact.request_id.empty() && progress.contains("request_id") && progress["request_id"].is_string())
        fact.request_id = progress["request_id"].get<std::string>();
    if (fact.page_size == 0) fact.page_size = json_u64_value(progress, "page_size", 0);
    fact.prefetch_ready_page_count = ready_page_count_from_progress(progress, fact.page_size);
    fact.prefetch_has_ongoing = json_bool_value(progress, "has_ongoing_prefetch", false);
    bool check_available = false;
    const bool check_return = json_bool_value(progress, "check_return", false, &check_available);
    if (check_available) {
        fact.prefetch_check_available = true;
        fact.prefetch_check_return = check_return;
    }

    auto pages_it = progress.find("operation_hash_pages");
    if (pages_it != progress.end()) {
        auto pages = pages_from_json_value(*pages_it);
        if (!pages.empty()) fact.pages = std::move(pages);
    }
}

bool HiCacheFactParser::role_requires_page_identity(const std::string & role, const TraceEvent & event) const {
    if (ends_with(role, "_start") || contains(role, "progress") || role == "prefetch_loaded_tokens" || role == "prefetch_decision") return false;
    if (ends_with(role, "_enqueue")) return false;
    if (role == "insert" && event.arg_u64("insert_tokens", 0) == 0 && event.arg_u64("value_tokens", 0) == 0) return false;
    if ((role == "prefetch_schedule" || role == "l3_prefetch_enqueue") && event.arg_u64("new_input_tokens", 0) < event.arg_u64("page_size", 1)) return false;
    if (role == "l3_hit_query") return event.arg_u64("hit_pages", 0) > 0 || event.arg_u64("num_pages", 0) > 0;
    if (role == "evict") return event.arg_u64("evicted_tokens", event.arg_u64("num_pages", 0)) > 0;
    static const std::unordered_set<std::string> exact_roles = {
        "insert",
        "load_back",
        "init_load_back",
        "write_backup",
        "write_storage_schedule",
        "l3_to_l2_transfer",
        "l3_prefetch_enqueue",
        "l2_to_l3_enqueue",
        "l2_to_l3_transfer",
        "prefetch_schedule",
        "remove_page",
        "lock_ref_inc",
        "lock_ref_dec",
    };
    if (exact_roles.count(role)) return true;
    if (contains(role, "to_l1") || contains(role, "to_l2") || contains(role, "to_l3")) return true;
    if (event.arg_u64("num_pages", 0) > 0 || event.arg_u64("hash_pages", 0) > 0) return true;
    return false;
}

} // namespace TraceGraph
