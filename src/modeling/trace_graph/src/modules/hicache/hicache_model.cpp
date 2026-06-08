#include "trace_graph/modules/hicache/hicache_model.hpp"

#include "trace_graph/core/dag_graph.hpp"
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

bool json_bool_value(const Json & object, const std::string & key, bool fallback, bool * observed = nullptr) {
    if (observed) *observed = false;
    if (!object.is_object()) return fallback;
    auto it = object.find(key);
    if (it == object.end() || it->is_null()) return fallback;
    if (observed) *observed = true;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0.0;
    if (it->is_string()) {
        auto text = lower(trim(it->get<std::string>()));
        if (text == "true" || text == "1" || text == "yes") return true;
        if (text == "false" || text == "0" || text == "no") return false;
        if (observed) *observed = false;
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

std::vector<std::string> sorted_vector(const std::set<std::string> & values) { return {values.begin(), values.end()}; }

bool is_vector_prefix(const std::vector<std::string> & prefix, const std::vector<std::string> & values) {
    if (prefix.size() > values.size()) return false;
    return std::equal(prefix.begin(), prefix.end(), values.begin());
}

// page size 变化时，这些角色描述的是 base run 中已经发生的 movement。
// 它们不能直接驱动 target state；缺少 target page identity 时应跳过并计数。
bool is_non_invariant_observed_role(const std::string & role) {
    if (role == "load_back" || role == "init_load_back" || role == "write_backup" || role == "write_storage_schedule" || role == "remove_page" ||
        role == "l3_hit_query")
        return true;
    // lock/ref 依赖当前 radix tree 的节点拆分和父链路径。page size what-if 下
    // base run 的 inc/dec 次数不是 target timeline 的不变量，只能用于同配置 replay。
    if (role == "lock_ref_inc" || role == "lock_ref_dec") return true;
    // l3_prefetch_enqueue 是 base run 中实际提交给 controller 的预取 enqueue。
    // page size what-if 下 target planned pages 应由 prefetch_schedule + target
    // policy 重新生成；不能消费 base enqueue 上按旧 last_hash 计算出的 target pages。
    if (role == "l3_prefetch_enqueue") return true;
    return contains(role, "to_l1") || contains(role, "to_l2") || contains(role, "to_l3");
}

bool is_capacity_observed_movement_role(const std::string & role) {
    if (role == "load_back" || role == "init_load_back" || role == "remove_page") return true;
    return contains(role, "to_l1");
}

bool is_prefetch_observed_movement_role(const std::string & role) {
    if (role == "l3_hit_query" || role == "prefetch_loaded_tokens" || role == "l3_prefetch_enqueue") return true;
    return contains(role, "l3_to_l2");
}

bool should_skip_non_invariant_target_movement(const HiCacheConfig & config, const HiCacheFact & fact) {
    // 同配置 replay 必须消费完整真实 movement；只有显式 target what-if
    // 才能跳过 base run 中已经发生、但不再是目标配置不变量的 movement。
    const bool explicit_capacity = config.l1_capacity_pages > 0 || config.l2_capacity_pages > 0;
    if (explicit_capacity && is_capacity_observed_movement_role(fact.role)) return true;
    if (config.write_policy == "write_back" && contains(fact.role, "l3_to_l2") && !config.write_back_prefetch_transfer_credit) return true;
    // best_effort 和显式 timeout 变体会改变 prefetch 终止点，base policy
    // 下观测到的 prefetch lock/ref 对数不是 target timeline 不变量。
    if ((config.prefetch_policy == "best_effort" || (config.prefetch_policy == "timeout" && config.prefetch_timeout_configured)) &&
        (fact.role == "lock_ref_inc" || fact.role == "lock_ref_dec"))
        return true;
    if (config.prefetch_policy != "observed" && is_prefetch_observed_movement_role(fact.role)) {
        // best_effort 的 ready pages 来自异步完成 credit。L3->L2 transfer
        // 是可用的 completion evidence，不能像普通 observed movement 一样
        // 全部跳过；是否真正更新状态由 HiCacheState 根据 planned pages 再判断。
        if ((config.prefetch_policy == "best_effort" || config.prefetch_policy == "timeout") && contains(fact.role, "l3_to_l2")) return false;
        return true;
    }
    if (config.write_policy != "observed" &&
        (fact.role == "write_backup" || fact.role == "write_storage_schedule" || contains(fact.role, "l1_to_l2") || contains(fact.role, "l2_to_l3")))
        return true;
    return false;
}

std::string join_set(const std::set<std::string> & values) {
    std::ostringstream os;
    bool first = true;
    for (const auto & value : values) {
        if (!first) os << ",";
        first = false;
        os << value;
    }
    return os.str();
}

std::string join_count_map(const std::unordered_map<std::string, uint64_t> & values) {
    std::map<std::string, uint64_t> ordered(values.begin(), values.end());
    std::ostringstream os;
    bool first = true;
    for (const auto & [key, count] : ordered) {
        if (!first) os << ",";
        first = false;
        os << key << ":" << count;
    }
    return os.str();
}

std::map<std::string, uint64_t> ordered_count_map(const std::unordered_map<std::string, uint64_t> & values) { return {values.begin(), values.end()}; }

Json transition_to_json(const HiCacheStateTransition & transition, bool emit_state_digests) {
    Json row = {
        {"transition_id", transition.transition_id},
        {"kind", transition.kind},
        {"role", transition.role},
        {"request_id", transition.request_id},
        {"operation_id", transition.operation_id},
        {"event_name", transition.event_name},
        {"cache_scope", transition.cache_scope},
        {"ts", transition.ts},
        {"source_event_index", transition.source_event_index},
        {"tier", transition.tier},
        {"pages", transition.pages},
    };
    if (emit_state_digests) {
        row["before_state_digest"] = transition.before_state_digest;
        row["after_state_digest"] = transition.after_state_digest;
    }
    return row;
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
    fact.requested_tokens = event.arg_u64("requested_tokens", 0);
    fact.evicted_tokens = event.arg_u64("evicted_tokens", 0);
    fact.pages = parse_page_identity(event);
    fact.source_pages = fact.pages;
    const bool insert_had_observed_pages = fact.role == "insert" && !fact.pages.empty();
    fact.target_pages = parse_page_arg(event, "target_page_identity");
    parse_prefetch_progress(event, fact);
    if (fact.role == "insert" && fact.prefix_len > 0 && fact.page_size > 0 && !fact.pages.empty()) {
        const size_t prefix_pages = std::min(fact.pages.size(), static_cast<size_t>(fact.prefix_len / fact.page_size));
        fact.pages.erase(fact.pages.begin(), fact.pages.begin() + static_cast<long>(prefix_pages));
    }
    fact.is_start = ends_with(event.name, "_start");
    fact.requires_page_identity = role_requires_page_identity(fact.role, event);
    if (fact.role == "insert" && insert_had_observed_pages && fact.pages.empty()) fact.requires_page_identity = false;
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

void HiCacheFactParser::parse_prefetch_progress(const TraceEvent & event, HiCacheFact & fact) const {
    if (fact.role != "prefetch_progress") return;

    if (event.has_arg("prefetch_done")) {
        const auto raw_done = lower(trim(event.arg("prefetch_done")));
        if (raw_done == "true" || raw_done == "1" || raw_done == "yes" || raw_done == "false" || raw_done == "0" || raw_done == "no") {
            fact.prefetch_check_observed = true;
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
    if (progress.contains("policy") && progress["policy"].is_string()) fact.prefetch_observed_policy = lower(trim(progress["policy"].get<std::string>()));
    if (fact.page_size == 0) fact.page_size = json_u64_value(progress, "page_size", 0);
    fact.prefetch_ready_page_count = ready_page_count_from_progress(progress, fact.page_size);
    fact.prefetch_has_ongoing = json_bool_value(progress, "has_ongoing_prefetch", false);
    bool observed = false;
    const bool check_return = json_bool_value(progress, "check_return", false, &observed);
    if (observed) {
        fact.prefetch_check_observed = true;
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

std::string HiCacheState::digest() const {
    std::ostringstream os;
    os << "l1=" << join_set(l1_) << ";l2=" << join_set(l2_) << ";l3=" << join_set(l3_) << ";dirty=" << join_set(dirty_) << ";backuped=" << join_set(backuped_)
       << ";evicted=" << join_set(evicted_) << ";prefetch_planned=" << join_set(prefetch_planned_) << ";prefetch_ready=" << join_set(prefetch_ready_)
       << ";prefetch_late=" << join_set(prefetch_late_) << ";prefetch_suppressed=" << join_set(prefetch_suppressed_) << ";locked=" << join_set(locked_)
       << ";hit_count=" << join_count_map(hit_count_by_scope_page_);
    return os.str();
}

HiCacheState::HiCacheState(HiCacheConfig config) : config_(std::move(config)) {}

std::vector<HiCacheStateTransition> HiCacheState::apply_fact(const HiCacheFact & fact, HiCacheSummary & summary) {
    std::vector<HiCacheStateTransition> transitions;
    if (fact.role == "lookup") {
        apply_lookup(fact, summary, transitions);
        return transitions;
    }
    if (fact.role == "insert") {
        auto insert_fact = fact;
        insert_fact.pages = target_insert_pages(fact);
        apply_insert(insert_fact, summary, transitions);
        mark_radix_known(insert_fact.pages);
        remember_leaf_group(insert_fact.pages);
        return transitions;
    }
    if (fact.role == "load_back" || fact.role == "init_load_back" || contains(fact.role, "l2_to_l1") || fact.tier_dst == "L1") {
        apply_load_to_l1(fact, summary, transitions);
        return transitions;
    }
    if (fact.role == "prefetch_schedule" || fact.role == "l3_prefetch_enqueue") {
        const auto pages = fact.role == "prefetch_schedule" ? target_prefetch_schedule_pages(fact) : fact.pages;
        remember_prefetch_schedule(fact, pages);
        for (const auto & page : pages) { mark_prefetch_planned(fact, summary, transitions, page); }
        return transitions;
    }
    if (fact.role == "prefetch_progress") {
        apply_prefetch_progress(fact, summary, transitions);
        return transitions;
    }
    if (fact.role == "l3_hit_query") {
        if (config_.prefetch_policy != "observed") {
            summary.skipped_non_invariant_events++;
            return transitions;
        }
        for (const auto & page : fact.pages) add_resident(fact, summary, transitions, "L3", page);
        return transitions;
    }
    if (fact.role == "l3_to_l2_transfer" || contains(fact.role, "l3_to_l2") || (fact.tier_src == "L3" && fact.tier_dst == "L2")) {
        apply_l3_to_l2(fact, summary, transitions, contains(fact.role, "prefetch") || fact.direction == "prefetch");
        return transitions;
    }
    if (fact.role == "write_backup" || contains(fact.role, "l1_to_l2") || (fact.tier_src == "L1" && fact.tier_dst == "L2")) {
        if (config_.write_policy == "write_back") return transitions;
        apply_write_to_l2(fact, summary, transitions);
        return transitions;
    }
    if (fact.role == "write_storage_schedule" || contains(fact.role, "l2_to_l3") || (fact.tier_src == "L2" && fact.tier_dst == "L3")) {
        if (config_.write_policy == "write_back") return transitions;
        apply_write_to_l3(fact, summary, transitions);
        return transitions;
    }
    if (fact.role == "evict") {
        apply_evict(fact, summary, transitions);
        return transitions;
    }
    if (fact.role == "evict_summary") {
        apply_policy_evict(fact, summary, transitions);
        return transitions;
    }
    if (fact.role == "remove_page") {
        apply_remove_page(fact, summary, transitions);
        return transitions;
    }
    if (fact.role == "lock_ref_inc") {
        apply_lock_ref(fact, summary, transitions, true);
        return transitions;
    }
    if (fact.role == "lock_ref_dec") {
        apply_lock_ref(fact, summary, transitions, false);
        return transitions;
    }
    apply_generic_tier_move(fact, summary, transitions);
    return transitions;
}

std::vector<HiCacheStateTransition> HiCacheState::finalize(HiCacheSummary & summary) {
    std::vector<HiCacheStateTransition> transitions;
    finalize_prefetch_policy(summary, transitions);
    return transitions;
}

std::set<std::string> * HiCacheState::tier_set(const std::string & tier) {
    if (tier == "L1") return &l1_;
    if (tier == "L2") return &l2_;
    if (tier == "L3") return &l3_;
    return nullptr;
}

bool HiCacheState::target_page_size_mismatch(const HiCacheFact & fact) const {
    return config_.page_size > 0 && fact.page_size > 0 && config_.page_size != fact.page_size;
}

bool HiCacheState::target_capacity_configured() const { return config_.l1_capacity_pages > 0 || config_.l2_capacity_pages > 0; }

bool HiCacheState::target_load_model_enabled(const HiCacheFact & fact) const { return target_page_size_mismatch(fact) || target_capacity_configured(); }

void HiCacheState::apply_lookup(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    if (fact.pages.empty()) return;
    last_lookup_pages_ = fact.pages;
    last_lookup_pages_by_scope_[fact.cache_scope] = fact.pages;
    if (!fact.request_id.empty()) pending_lookup_pages_by_request_[scoped_request_key(fact)] = fact.pages;
    for (const auto & page : fact.pages) {
        if (l1_.count(page) > 0) {
            touch_page("L1", page);
            if (l2_.count(page) > 0) touch_page("L2", page);
            continue;
        }
        // 同配置 replay 中真实 load_back / transfer 事件会更新 resident。
        // target what-if 跳过 observed movement 后，lookup 必须根据目标
        // resident set 自己推导命中后的状态变化。
        if (!target_load_model_enabled(fact)) continue;
        if (l2_.count(page) > 0) {
            touch_page("L2", page);
            add_resident(fact, summary, transitions, "L1", page);
            continue;
        }
        if (l3_.count(page) > 0) {
            add_resident(fact, summary, transitions, "L2", page);
            add_resident(fact, summary, transitions, "L1", page);
        }
    }
    enforce_capacity(fact, summary, transitions, "L2");
    enforce_capacity(fact, summary, transitions, "L1");
}

std::vector<std::string> HiCacheState::target_insert_pages(const HiCacheFact & fact) const {
    // page size what-if 下，base insert 的 prefix_len/page set 不是 target
    // radix tree 的真实 split 结果。这里用最近一次 target lookup path 和
    // 已知 radix pages 做最小 prefix match，先把 insert 语义从 base trace
    // movement 中剥离出来。完整 node split 后续继续在 HiCachePolicyModel 中补。
    if (!target_page_size_mismatch(fact)) return fact.pages;

    auto lookup_it = fact.request_id.empty() ? pending_lookup_pages_by_request_.end() : pending_lookup_pages_by_request_.find(scoped_request_key(fact));
    auto scope_lookup_it = last_lookup_pages_by_scope_.find(fact.cache_scope);
    const auto & fallback_lookup_pages = scope_lookup_it == last_lookup_pages_by_scope_.end() ? last_lookup_pages_ : scope_lookup_it->second;
    const auto & lookup_pages = lookup_it == pending_lookup_pages_by_request_.end() ? fallback_lookup_pages : lookup_it->second;
    if (lookup_pages.empty()) return fact.pages;

    size_t prefix_pages = 0;
    while (prefix_pages < lookup_pages.size() && radix_known_pages_.count(lookup_pages[prefix_pages]) > 0) ++prefix_pages;
    return {lookup_pages.begin() + static_cast<long>(prefix_pages), lookup_pages.end()};
}

void HiCacheState::mark_radix_known(const std::vector<std::string> & pages) {
    for (const auto & page : pages) {
        if (!page.empty()) radix_known_pages_.insert(page);
    }
}

void HiCacheState::remember_leaf_group(const std::vector<std::string> & pages) {
    if (pages.empty()) return;
    std::vector<std::string> group;
    group.reserve(pages.size());
    for (const auto & page : pages) {
        if (!page.empty()) group.push_back(page);
    }
    if (group.empty()) return;
    for (const auto & page : group) leaf_group_by_page_[page] = group;
}

void HiCacheState::apply_insert(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    auto dst = fact.tier_dst.empty() ? "L1" : fact.tier_dst;
    if (target_capacity_configured() && !fact.pages.empty()) {
        auto missing_pages = [](const std::set<std::string> & resident, const std::vector<std::string> & pages) {
            uint64_t count = 0;
            for (const auto & page : pages) {
                if (!resident.count(page)) ++count;
            }
            return count;
        };
        auto pages_to_free = [](size_t current, uint64_t incoming, uint64_t capacity) {
            if (capacity == 0 || current + incoming <= capacity) return uint64_t{0};
            return static_cast<uint64_t>(current + incoming - capacity);
        };
        if (dst == "L1") {
            const uint64_t free_l1 = pages_to_free(l1_.size(), missing_pages(l1_, fact.pages), config_.l1_capacity_pages);
            evict_lru_pages(fact, summary, transitions, "L1", free_l1);
        }
        if (config_.write_policy == "write_through") {
            const uint64_t free_l2 = pages_to_free(l2_.size(), missing_pages(l2_, fact.pages), config_.l2_capacity_pages);
            evict_lru_pages(fact, summary, transitions, "L2", free_l2);
        }
    }
    for (const auto & page : fact.pages) {
        add_resident(fact, summary, transitions, dst, page);
        if (config_.write_policy == "write_through") {
            add_resident(fact, summary, transitions, "L2", page);
            // 显式 write-through what-if 会跳过 base trace 中观测到的
            // write_storage movement，因此 insert 必须在目标状态中同步补出
            // storage readable set；否则后续 lookup 无法从 L3 推导 target load。
            add_resident(fact, summary, transitions, "L3", page);
            mark_backuped(fact, summary, transitions, page);
            clear_dirty(fact, summary, transitions, page);
        }
        else {
            if (fact.dirty && !backuped_.count(page)) mark_dirty(fact, summary, transitions, page);
            if (fact.backuped) mark_backuped(fact, summary, transitions, page);
        }
    }
    apply_write_policy_hit_counts(fact, summary, transitions);
    enforce_capacity(fact, summary, transitions, dst);
    enforce_capacity(fact, summary, transitions, "L2");
}

uint64_t HiCacheState::target_write_through_threshold() const {
    if (config_.write_through_threshold > 0) return config_.write_through_threshold;
    if (config_.write_policy == "write_through") return 1;
    if (config_.write_policy == "write_through_selective") return 2;
    return 0;
}

bool HiCacheState::target_write_count_enabled() const { return config_.write_policy == "write_through" || config_.write_policy == "write_through_selective"; }

std::string HiCacheState::scoped_request_key(const HiCacheFact & fact) const {
    if (fact.request_id.empty()) return "";
    return (fact.cache_scope.empty() ? std::string("-1") : fact.cache_scope) + ":" + fact.request_id;
}

std::string HiCacheState::scoped_page_key(const HiCacheFact & fact, const std::string & page) const {
    return (fact.cache_scope.empty() ? std::string("-1") : fact.cache_scope) + ":" + page;
}

bool HiCacheState::page_locked_in_any_scope(const std::string & page) const {
    const auto suffix = ":" + page;
    for (const auto & [key, count] : lock_count_by_scope_page_) {
        if (count > 0 && key.size() >= suffix.size() && key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) return true;
    }
    return false;
}

std::vector<std::string> HiCacheState::write_policy_hit_pages_for_insert(const HiCacheFact & fact) const {
    std::vector<std::string> pages;
    if (!fact.request_id.empty()) {
        auto lookup_it = pending_lookup_pages_by_request_.find(scoped_request_key(fact));
        if (lookup_it != pending_lookup_pages_by_request_.end()) pages = lookup_it->second;
    }
    if (pages.empty()) {
        auto scope_lookup_it = last_lookup_pages_by_scope_.find(fact.cache_scope);
        if (scope_lookup_it != last_lookup_pages_by_scope_.end()) pages = scope_lookup_it->second;
    }
    if (pages.empty()) pages = fact.pages;

    std::set<std::string> seen(pages.begin(), pages.end());
    for (const auto & page : fact.pages) {
        if (!page.empty() && seen.insert(page).second) pages.push_back(page);
    }
    return pages;
}

bool HiCacheState::write_policy_prefix_backup_ready(const std::vector<std::string> & pages, size_t index) const {
    for (size_t prefix_index = 0; prefix_index < index; ++prefix_index) {
        const auto & prefix_page = pages[prefix_index];
        if (!prefix_page.empty() && backuped_.count(prefix_page) == 0) return false;
    }
    return true;
}

void HiCacheState::apply_write_policy_hit_counts(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    if (!target_write_count_enabled() || fact.chunked) return;
    const uint64_t threshold = target_write_through_threshold();
    if (threshold == 0) return;

    const auto pages = write_policy_hit_pages_for_insert(fact);
    std::set<std::string> counted;
    for (size_t index = 0; index < pages.size(); ++index) {
        const auto & page = pages[index];
        if (page.empty() || !counted.insert(page).second) continue;
        if (l1_.count(page) == 0 && radix_known_pages_.count(page) == 0) continue;

        const auto before = transition_state_digest();
        const auto scoped_key = scoped_page_key(fact, page);
        hit_count_by_scope_page_[scoped_key]++;
        record_transition(fact, summary, transitions, "increment_hit_count", "", page, before);

        if (backuped_.count(page) > 0) continue;
        if (hit_count_by_scope_page_[scoped_key] < threshold) continue;
        if (!write_policy_prefix_backup_ready(pages, index)) continue;
        backup_page_for_write_policy(fact, summary, transitions, page);
    }
}

void HiCacheState::backup_page_for_write_policy(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                                const std::string & page) {
    add_resident(fact, summary, transitions, "L1", page);
    add_resident(fact, summary, transitions, "L2", page);
    // target prediction 会跳过 base trace 的 write_storage movement；这里把
    // 达到阈值的 page 同步标成 storage readable，后续 L3->L2 load 才有状态来源。
    add_resident(fact, summary, transitions, "L3", page);
    mark_backuped(fact, summary, transitions, page);
    clear_dirty(fact, summary, transitions, page);
    enforce_capacity(fact, summary, transitions, "L2");
}

void HiCacheState::apply_load_to_l1(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    if (target_capacity_configured() &&
        (fact.role == "load_back" || fact.role == "init_load_back" || contains(fact.role, "l2_to_l1") || fact.tier_dst == "L1")) {
        summary.skipped_non_invariant_events++;
        return;
    }
    auto src = fact.tier_src.empty() ? "L2" : fact.tier_src;
    if (target_capacity_configured() && config_.l1_capacity_pages > 0 && l1_.size() + fact.pages.size() > config_.l1_capacity_pages)
        evict_lru_pages(fact, summary, transitions, "L1", fact.pages.size());
    for (const auto & page : fact.pages) {
        add_resident(fact, summary, transitions, src, page);
        add_resident(fact, summary, transitions, "L1", page);
    }
    enforce_capacity(fact, summary, transitions, src);
    enforce_capacity(fact, summary, transitions, "L1");
}

void HiCacheState::apply_l3_to_l2(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, bool ready) {
    const auto pages = target_prefetch_completion_pages(fact);
    bool planned_prefetch_transfer = false;
    for (const auto & page : pages) {
        if (prefetch_planned_.count(page) > 0) {
            planned_prefetch_transfer = true;
            break;
        }
    }
    const bool best_effort_credit = config_.prefetch_policy == "best_effort" && planned_prefetch_transfer;
    const bool timeout_credit = config_.prefetch_policy == "timeout" && planned_prefetch_transfer;
    if (config_.prefetch_policy != "observed" && !best_effort_credit && !timeout_credit) {
        summary.skipped_non_invariant_events++;
        return;
    }
    for (const auto & page : pages) {
        add_resident(fact, summary, transitions, "L3", page);
        add_resident(fact, summary, transitions, "L2", page);
        if (ready || prefetch_planned_.count(page) > 0) mark_prefetch_ready(fact, summary, transitions, page);
    }
    enforce_capacity(fact, summary, transitions, "L2");
}

void HiCacheState::apply_prefetch_progress(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    const bool terminal_empty_progress = fact.pages.empty() && fact.prefetch_check_observed && fact.prefetch_check_return && !fact.prefetch_has_ongoing;
    const std::string effective_prefetch_policy =
        config_.prefetch_policy == "observed" && !fact.prefetch_observed_policy.empty() ? fact.prefetch_observed_policy : config_.prefetch_policy;
    const bool has_progress_payload = fact.prefetch_progress_evidence;
    if (!fact.request_id.empty() && terminal_empty_progress && effective_prefetch_policy == "best_effort") {
        auto pending_it = pending_prefetch_pages_by_request_.find(scoped_request_key(fact));
        if (pending_it != pending_prefetch_pages_by_request_.end()) {
            for (const auto & page : pending_it->second) mark_prefetch_suppressed(fact, summary, transitions, page);
        }
    }
    if (!fact.request_id.empty() && terminated_prefetch_requests_.count(scoped_request_key(fact)) > 0) return;

    const auto pages = prefetch_pages_for_fact(fact);
    const bool has_operation_progress_pages =
        !fact.pages.empty() || (!fact.request_id.empty() &&
                                latest_prefetch_progress_pages_by_request_.find(scoped_request_key(fact)) != latest_prefetch_progress_pages_by_request_.end());
    if (!fact.request_id.empty() && !fact.pages.empty()) latest_prefetch_progress_pages_by_request_[scoped_request_key(fact)] = fact.pages;
    if (pages.empty()) return;

    if (!fact.request_id.empty()) remember_prefetch_pages(fact, pages);

    if (!has_operation_progress_pages && fact.prefetch_has_ongoing && !fact.prefetch_check_return) return;

    // 没有显式 progress payload 时，只保留 pending/终止语义，不把 progress
    // 观测反推出 ready page。否则纯 prefetch_done 事件会把 page64 strict
    // prediction 里的 ready 集合过度膨胀。
    const bool progress_ready_credit_allowed = !(config_.write_policy == "write_back" && config_.prefetch_policy != "observed");
    const auto ready_count = has_progress_payload && progress_ready_credit_allowed ? std::min<uint64_t>(fact.prefetch_ready_page_count, pages.size()) : 0;
    if (ready_count > 0) {
        for (size_t index = 0; index < static_cast<size_t>(ready_count); ++index) {
            const auto & page = pages[index];
            add_resident(fact, summary, transitions, "L3", page);
            add_resident(fact, summary, transitions, "L2", page);
            mark_prefetch_ready(fact, summary, transitions, page);
        }
        enforce_capacity(fact, summary, transitions, "L2");
    }

    if (!should_terminate_prefetch_at_progress(fact, pages, ready_count)) return;
    if (!fact.request_id.empty() && config_.prefetch_policy != "observed") terminated_prefetch_requests_.insert(scoped_request_key(fact));
    if (config_.prefetch_policy == "wait_complete") return;

    if (!has_operation_progress_pages && !fact.prefetch_has_ongoing && ready_count == 0) {
        for (const auto & page : pages) mark_prefetch_suppressed(fact, summary, transitions, page);
        return;
    }
    for (size_t index = static_cast<size_t>(ready_count); index < pages.size(); ++index) { mark_prefetch_late(fact, summary, transitions, pages[index]); }
}

void HiCacheState::apply_write_to_l2(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    for (const auto & page : fact.pages) {
        add_resident(fact, summary, transitions, "L1", page);
        add_resident(fact, summary, transitions, "L2", page);
        mark_backuped(fact, summary, transitions, page);
        clear_dirty(fact, summary, transitions, page);
    }
    enforce_capacity(fact, summary, transitions, "L2");
}

void HiCacheState::apply_write_to_l3(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    for (const auto & page : fact.pages) {
        add_resident(fact, summary, transitions, "L2", page);
        add_resident(fact, summary, transitions, "L3", page);
        mark_backuped(fact, summary, transitions, page);
        clear_dirty(fact, summary, transitions, page);
    }
    enforce_capacity(fact, summary, transitions, "L2");
}

void HiCacheState::apply_evict(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    for (const auto & page : fact.pages) {
        if (dirty_.count(page)) {
            summary.dirty_eviction_events++;
            write_back_dirty_page(fact, summary, transitions, page);
        }
        if (!fact.tier_src.empty())
            remove_resident(fact, summary, transitions, fact.tier_src, page);
        else if (l1_.count(page))
            remove_resident(fact, summary, transitions, "L1", page);
        else if (l2_.count(page))
            remove_resident(fact, summary, transitions, "L2", page);
        else if (l3_.count(page))
            remove_resident(fact, summary, transitions, "L3", page);
        mark_evicted(fact, summary, transitions, page);
    }
}

void HiCacheState::apply_policy_evict(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    // 同配置 replay 中后续 remove_page 事件会给出真实被删 page，不能用
    // evict_summary 再推一次。page-size what-if 会跳过 observed movement，
    // 因此这里把 evict_summary 作为 target eviction policy 的触发点。
    if (!target_page_size_mismatch(fact)) {
        if (target_capacity_configured()) summary.skipped_non_invariant_events++;
        return;
    }
    const uint64_t target_page_size = config_.page_size > 0 ? config_.page_size : fact.page_size;
    if (target_page_size == 0 || fact.requested_tokens == 0) return;

    const std::string dedupe_key = std::to_string(fact.requested_tokens) + ":" + std::to_string(fact.evicted_tokens);
    if (dedupe_key == last_policy_evict_key_ && fact.ts >= last_policy_evict_ts_ && fact.ts - last_policy_evict_ts_ < 1000000) return;
    last_policy_evict_key_ = dedupe_key;
    last_policy_evict_ts_ = fact.ts;

    const uint64_t pages_to_free = (fact.requested_tokens + target_page_size - 1) / target_page_size;
    evict_lru_pages(fact, summary, transitions, "L1", pages_to_free);
}

void HiCacheState::apply_remove_page(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    if (target_capacity_configured()) {
        summary.skipped_non_invariant_events++;
        return;
    }
    const auto tier = fact.tier_src.empty() ? "L1" : fact.tier_src;
    for (const auto & page : fact.pages) {
        if (tier == "L1" && config_.write_policy == "write_back" && dirty_.count(page)) {
            summary.dirty_eviction_events++;
            write_back_dirty_page(fact, summary, transitions, page);
        }
        remove_resident(fact, summary, transitions, tier, page);
        if (tier == "L2") {
            clear_backuped(fact, summary, transitions, page);
            clear_evicted(fact, summary, transitions, page);
        }
        else if (tier == "L1") {
            mark_evicted(fact, summary, transitions, page);
        }
    }
}

void HiCacheState::apply_lock_ref(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, bool increment) {
    summary.lock_state_events++;
    for (const auto & page : fact.pages) {
        const auto key = scoped_page_key(fact, page);
        if (increment) {
            lock_count_by_scope_page_[key]++;
            mark_locked(fact, summary, transitions, page);
            continue;
        }
        auto it = lock_count_by_scope_page_.find(key);
        if (it != lock_count_by_scope_page_.end() && it->second > 0) {
            it->second--;
            if (it->second == 0) {
                lock_count_by_scope_page_.erase(it);
                if (!page_locked_in_any_scope(page)) clear_locked(fact, summary, transitions, page);
            }
        }
        else {
            if (!page_locked_in_any_scope(page)) clear_locked(fact, summary, transitions, page);
        }
    }
}

void HiCacheState::apply_generic_tier_move(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    for (const auto & page : fact.pages) {
        if (!fact.tier_src.empty()) add_resident(fact, summary, transitions, fact.tier_src, page);
        if (!fact.tier_dst.empty()) add_resident(fact, summary, transitions, fact.tier_dst, page);
        if (fact.direction == "write" || fact.tier_dst == "L3") {
            mark_backuped(fact, summary, transitions, page);
            clear_dirty(fact, summary, transitions, page);
        }
        if (fact.direction == "evict" && !fact.tier_src.empty()) remove_resident(fact, summary, transitions, fact.tier_src, page);
    }
    if (!fact.tier_src.empty()) enforce_capacity(fact, summary, transitions, fact.tier_src);
    if (!fact.tier_dst.empty()) enforce_capacity(fact, summary, transitions, fact.tier_dst);
}

void HiCacheState::add_resident(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & tier,
                                const std::string & page) {
    auto * pages = tier_set(tier);
    if (!pages) return;
    auto before = transition_state_digest();
    touch_page(tier, page);
    if (pages->insert(page).second) {
        evicted_.erase(page);
        record_transition(fact, summary, transitions, "add_" + lower(tier) + "_resident", tier, page, before);
    }
    if (tier == "L2") mark_backuped(fact, summary, transitions, page);
}

void HiCacheState::remove_resident(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                   const std::string & tier, const std::string & page) {
    auto * pages = tier_set(tier);
    if (!pages) return;
    auto before = transition_state_digest();
    if (pages->erase(page) > 0) {
        record_transition(fact, summary, transitions, "remove_" + lower(tier) + "_resident", tier, page, before);
        if (tier == "L2") clear_backuped(fact, summary, transitions, page);
    }
}

void HiCacheState::mark_dirty(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & page) {
    auto before = transition_state_digest();
    if (dirty_.insert(page).second) record_transition(fact, summary, transitions, "mark_dirty", "", page, before);
}

void HiCacheState::clear_dirty(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                               const std::string & page) {
    auto before = transition_state_digest();
    if (dirty_.erase(page) > 0) record_transition(fact, summary, transitions, "clear_dirty", "", page, before);
}

void HiCacheState::mark_backuped(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                 const std::string & page) {
    auto before = transition_state_digest();
    if (backuped_.insert(page).second) record_transition(fact, summary, transitions, "mark_backuped", "", page, before);
    // SGLang 的 TreeNode.backuped 表示 host_value 已存在；一旦备份成立，
    // 该页不应继续留在 dirty 集合中。
    clear_dirty(fact, summary, transitions, page);
}

void HiCacheState::clear_backuped(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                  const std::string & page) {
    auto before = transition_state_digest();
    if (backuped_.erase(page) > 0) record_transition(fact, summary, transitions, "clear_backuped", "", page, before);
}

void HiCacheState::mark_evicted(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                const std::string & page) {
    auto before = transition_state_digest();
    if (evicted_.insert(page).second) record_transition(fact, summary, transitions, "mark_evicted", "", page, before);
}

void HiCacheState::clear_evicted(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                 const std::string & page) {
    auto before = transition_state_digest();
    if (evicted_.erase(page) > 0) record_transition(fact, summary, transitions, "clear_evicted", "", page, before);
}

void HiCacheState::mark_locked(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                               const std::string & page) {
    auto before = transition_state_digest();
    if (locked_.insert(page).second) record_transition(fact, summary, transitions, "mark_locked", "", page, before);
}

void HiCacheState::clear_locked(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                const std::string & page) {
    auto before = transition_state_digest();
    if (locked_.erase(page) > 0) record_transition(fact, summary, transitions, "clear_locked", "", page, before);
}

void HiCacheState::mark_prefetch_planned(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                         const std::string & page) {
    auto before = transition_state_digest();
    if (prefetch_planned_.insert(page).second) record_transition(fact, summary, transitions, "mark_prefetch_planned", "", page, before);
}

void HiCacheState::mark_prefetch_ready(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                       const std::string & page) {
    auto before = transition_state_digest();
    if (prefetch_ready_.insert(page).second) record_transition(fact, summary, transitions, "mark_prefetch_ready", "", page, before);
    prefetch_late_.erase(page);
    prefetch_suppressed_.erase(page);
}

void HiCacheState::mark_prefetch_late(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                      const std::string & page) {
    if (prefetch_ready_.count(page) > 0) return;
    auto before = transition_state_digest();
    if (prefetch_late_.insert(page).second) record_transition(fact, summary, transitions, "mark_prefetch_late", "", page, before);
}

void HiCacheState::mark_prefetch_suppressed(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                            const std::string & page) {
    if (prefetch_ready_.count(page) > 0) return;
    auto before = transition_state_digest();
    if (prefetch_suppressed_.insert(page).second) record_transition(fact, summary, transitions, "mark_prefetch_suppressed", "", page, before);
}

void HiCacheState::remember_prefetch_pages(const HiCacheFact & fact, const std::vector<std::string> & pages) {
    if (fact.request_id.empty() || pages.empty()) return;
    auto & remembered = pending_prefetch_pages_by_request_[scoped_request_key(fact)];
    std::unordered_set<std::string> seen(remembered.begin(), remembered.end());
    for (const auto & page : pages) {
        if (!page.empty() && seen.insert(page).second) remembered.push_back(page);
    }
}

void HiCacheState::remember_prefetch_schedule(const HiCacheFact & fact, const std::vector<std::string> & pages) {
    remember_prefetch_pages(fact, pages);
    if (fact.request_id.empty()) return;
    const auto key = scoped_request_key(fact);
    prefetch_schedule_ts_by_request_[key] = fact.ts;
    latest_prefetch_schedule_pages_by_request_[key] = pages;
    if (fact.role == "prefetch_schedule" && target_page_size_mismatch(fact) && !fact.source_pages.empty()) {
        auto & source_page_count = prefetch_schedule_source_page_count_by_request_[key];
        source_page_count = std::max<uint64_t>(source_page_count, fact.source_pages.size());
    }
}

std::vector<std::string> HiCacheState::target_prefetch_schedule_pages(const HiCacheFact & fact) const {
    if (!target_page_size_mismatch(fact)) return fact.pages;
    if (fact.request_id.empty() || fact.new_input_tokens == 0 || config_.page_size == 0) return fact.pages;

    const auto lookup_it = pending_lookup_pages_by_request_.find(scoped_request_key(fact));
    if (lookup_it == pending_lookup_pages_by_request_.end() || lookup_it->second.empty()) return fact.pages;

    // page size what-if 下，prefetch_from_storage 的 prefix_keys 可能为空；
    // probe 只能生成没有 parent hash 的 target_page_identity。lookup 已经
    // 暴露了同 request 在目标 page size 下的完整 path，这里按目标 page
    // size 从 path 尾部截出 new_input_tokens 对应的完整 suffix pages。
    const size_t suffix_pages = static_cast<size_t>(fact.new_input_tokens / config_.page_size);
    if (suffix_pages == 0 || lookup_it->second.size() < suffix_pages) return fact.pages;
    return {lookup_it->second.end() - static_cast<long>(suffix_pages), lookup_it->second.end()};
}

std::vector<std::string> HiCacheState::target_prefetch_completion_pages(const HiCacheFact & fact) const {
    if (!target_page_size_mismatch(fact) || fact.request_id.empty()) return fact.pages;
    if (config_.prefetch_policy != "best_effort" && config_.prefetch_policy != "timeout") return fact.pages;

    const auto key = scoped_request_key(fact);
    const auto schedule_it = latest_prefetch_schedule_pages_by_request_.find(key);
    if (schedule_it == latest_prefetch_schedule_pages_by_request_.end() || schedule_it->second.empty()) return fact.pages;

    const auto source_count_it = prefetch_schedule_source_page_count_by_request_.find(key);
    if (source_count_it == prefetch_schedule_source_page_count_by_request_.end() || source_count_it->second == 0) return fact.pages;
    if (fact.source_pages.size() < source_count_it->second) return fact.pages;

    // page size what-if 下，base transfer 的 target_page_identity 只覆盖
    // base operation token 数能整除出的 target pages，也可能因为 base
    // prefetch 的 last_hash / parent context 不同而和 target schedule
    // identity 不完全一致。只要 source pages 已覆盖该 request 的 schedule
    // source pages，completion credit 应归到目标配置计划出的 suffix pages。
    return schedule_it->second;
}

std::vector<std::string> HiCacheState::prefetch_pages_for_fact(const HiCacheFact & fact) const {
    if (!fact.pages.empty()) return fact.pages;
    if (fact.request_id.empty()) return {};
    const auto key = scoped_request_key(fact);
    auto progress_it = latest_prefetch_progress_pages_by_request_.find(key);
    if (progress_it != latest_prefetch_progress_pages_by_request_.end() && !progress_it->second.empty()) return progress_it->second;
    auto pending_it = pending_prefetch_pages_by_request_.find(key);
    if (pending_it != pending_prefetch_pages_by_request_.end()) return pending_it->second;
    return {};
}

void HiCacheState::finalize_prefetch_policy(HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    HiCacheFact fact;
    fact.role = "prefetch_finalize";
    fact.event_name = "hicache_prefetch_finalize";
    if (config_.prefetch_policy == "best_effort" || config_.prefetch_policy == "wait_complete") {
        // best_effort 在 run 结束时不会再等待未完成预取；wait_complete
        // 在真实 trace 中也可能只产生 planned 而没有可完成的 storage
        // operation。对当前 run 来说，这些 planned 但未 ready 的 page
        // 应作为 suppressed final state 暴露，方便和 oracle 对齐。
        for (const auto & page : prefetch_planned_) {
            if (prefetch_ready_.count(page) > 0) continue;
            if (prefetch_suppressed_.count(page) > 0) continue;
            mark_prefetch_suppressed(fact, summary, transitions, page);
        }
        return;
    }
    if (config_.prefetch_policy != "timeout") return;

    // timeout 不能把所有 planned page 都在尾部强行归为 suppressed。
    // 普通 timeout run 可能只暴露 planned/ready，不暴露 timeout 终止；
    // 只有已经由 progress evidence 证明终止的 request，才补齐其未 ready
    // page 的 suppressed final state。
    for (const auto & request_key : terminated_prefetch_requests_) {
        auto it = pending_prefetch_pages_by_request_.find(request_key);
        if (it == pending_prefetch_pages_by_request_.end()) continue;
        for (const auto & page : it->second) {
            if (prefetch_ready_.count(page) > 0) continue;
            if (prefetch_suppressed_.count(page) > 0) continue;
            mark_prefetch_suppressed(fact, summary, transitions, page);
        }
    }
}

bool HiCacheState::should_terminate_prefetch_at_progress(const HiCacheFact & fact, const std::vector<std::string> & pages, uint64_t ready_count) const {
    const auto policy = config_.prefetch_policy.empty() ? std::string("observed") : config_.prefetch_policy;
    if (policy == "best_effort") {
        // SGLang best_effort 在第一次 check_prefetch_progress 时允许 terminate。
        // 因此 prediction 不能等 base trace 的 check_return=true。
        return fact.prefetch_progress_evidence || fact.prefetch_check_observed;
    }
    if (policy == "wait_complete") { return !pages.empty() && ready_count >= pages.size(); }
    if (policy == "timeout") {
        if (!pages.empty() && ready_count >= pages.size()) return true;
        if (fact.prefetch_check_observed && fact.prefetch_check_return && !fact.prefetch_has_ongoing) return true;
        if (prefetch_timeout_reached(fact, pages)) return true;
        // 没有显式 target timeout 参数时保留 replay 语义，跟随真实 check 返回。
        return !config_.prefetch_timeout_configured && fact.prefetch_check_observed && fact.prefetch_check_return;
    }
    return fact.prefetch_check_observed && fact.prefetch_check_return;
}

bool HiCacheState::prefetch_timeout_reached(const HiCacheFact & fact, const std::vector<std::string> & pages) const {
    if (!config_.prefetch_timeout_configured || fact.request_id.empty()) return false;
    auto schedule_it = prefetch_schedule_ts_by_request_.find(scoped_request_key(fact));
    if (schedule_it == prefetch_schedule_ts_by_request_.end()) return false;
    if (fact.ts < schedule_it->second) return false;

    const uint64_t page_size = config_.page_size > 0 ? config_.page_size : fact.page_size;
    const uint64_t token_count = page_size > 0 ? static_cast<uint64_t>(pages.size()) * page_size : 0;
    double timeout_sec = config_.prefetch_timeout_base_sec + config_.prefetch_timeout_per_ki_token_sec * static_cast<double>(token_count) / 1024.0;
    if (config_.prefetch_timeout_max_sec > 0.0) timeout_sec = std::min(timeout_sec, config_.prefetch_timeout_max_sec);
    if (timeout_sec < 0.0) timeout_sec = 0.0;

    const uint64_t elapsed_us = fact.ts - schedule_it->second;
    const double elapsed_sec = static_cast<double>(elapsed_us) / 1000000.0;
    return elapsed_sec >= timeout_sec;
}

void HiCacheState::touch_page(const std::string & tier, const std::string & page) {
    auto * order = tier == "L1" ? &l1_touch_order_ : (tier == "L2" ? &l2_touch_order_ : nullptr);
    if (!order) return;
    order->erase(std::remove(order->begin(), order->end(), page), order->end());
    order->push_back(page);
}

void HiCacheState::write_back_dirty_page(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                         const std::string & page) {
    add_resident(fact, summary, transitions, "L2", page);
    clear_dirty(fact, summary, transitions, page);
}

std::map<std::string, uint64_t> HiCacheState::page_hit_count_summary() const {
    std::map<std::string, uint64_t> result;
    for (const auto & [scoped_key, count] : hit_count_by_scope_page_) {
        const auto separator = scoped_key.find(':');
        const auto page = separator == std::string::npos ? scoped_key : scoped_key.substr(separator + 1);
        auto & current = result[page];
        current = std::max(current, count);
    }
    return result;
}

void HiCacheState::evict_lru_pages(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                   const std::string & tier, uint64_t page_count) {
    auto * pages = tier_set(tier);
    auto * order = tier == "L1" ? &l1_touch_order_ : (tier == "L2" ? &l2_touch_order_ : nullptr);
    if (!pages || !order || page_count == 0) return;

    uint64_t removed = 0;
    while (removed < page_count && !order->empty()) {
        auto victim = order->front();
        order->erase(order->begin());
        if (!pages->count(victim)) continue;
        auto group_it = leaf_group_by_page_.find(victim);
        const bool use_leaf_group = target_page_size_mismatch(fact) && group_it != leaf_group_by_page_.end();
        const auto victims = use_leaf_group ? group_it->second : std::vector<std::string>{victim};
        for (const auto & page : victims) {
            if (!pages->count(page)) continue;
            order->erase(std::remove(order->begin(), order->end(), page), order->end());
            if (dirty_.count(page)) {
                summary.dirty_eviction_events++;
                write_back_dirty_page(fact, summary, transitions, page);
            }
            remove_resident(fact, summary, transitions, tier, page);
            if (tier == "L2")
                clear_evicted(fact, summary, transitions, page);
            else
                mark_evicted(fact, summary, transitions, page);
            removed++;
        }
    }
}

void HiCacheState::enforce_capacity(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                    const std::string & tier) {
    auto capacity = tier == "L1" ? config_.l1_capacity_pages : (tier == "L2" ? config_.l2_capacity_pages : 0);
    if (capacity == 0) return;
    auto * pages = tier_set(tier);
    auto * order = tier == "L1" ? &l1_touch_order_ : (tier == "L2" ? &l2_touch_order_ : nullptr);
    if (!pages || !order) return;
    while (pages->size() > capacity && !order->empty()) { evict_lru_pages(fact, summary, transitions, tier, pages->size() - capacity); }
}

std::string HiCacheState::transition_state_digest() const { return config_.emit_state_digests ? digest() : std::string{}; }

void HiCacheState::record_transition(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                     const std::string & kind, const std::string & tier, const std::string & page, const std::string & before_digest) {
    summary.state_transition_count++;
    summary.transitions_by_kind[kind]++;
    HiCacheStateTransition transition;
    transition.transition_id = "hicache_transition_" + std::to_string(summary.state_transition_count);
    transition.kind = kind;
    transition.role = fact.role;
    transition.request_id = fact.request_id;
    transition.operation_id = fact.operation_id;
    transition.event_name = fact.event_name;
    transition.cache_scope = fact.cache_scope;
    transition.ts = fact.ts;
    transition.source_event_index = fact.source_event_index;
    transition.tier = tier;
    transition.pages = {page};
    transition.before_state_digest = before_digest;
    transition.after_state_digest = transition_state_digest();
    transitions.push_back(std::move(transition));
}

HiCacheStateModel::HiCacheStateModel(HiCacheConfig config) : config_(std::move(config)), state_(config_) {}

HiCacheSummary HiCacheStateModel::run(DagGraph & graph) {
    HiCacheSummary summary;
    summary.target_config = config_;
    if (!config_.enabled) {
        summary.status = "disabled";
        return summary;
    }

    std::vector<HiCacheFact> facts;
    for (const auto & node : graph.nodes()) {
        const auto & event = graph.event_for_node(node.id);
        if (!fact_parser_.is_hicache_event(event)) continue;

        auto fact = fact_parser_.parse(node.id, event);
        summary.input_hicache_events++;
        summary.events_by_role[fact.role]++;
        facts.push_back(std::move(fact));
    }

    // Python probe 用 X event 表达 start/end 时，merged trace 中同 timestamp
    // 可能出现 end 在 start 前。state model 需要按调用逻辑顺序消费事实，否则
    // prefetch_progress_end 会在 operation progress evidence 之前被误判为 suppressed。
    std::stable_sort(facts.begin(), facts.end(), [](const HiCacheFact & left, const HiCacheFact & right) {
        if (left.ts != right.ts) return left.ts < right.ts;
        if (left.is_start != right.is_start) return left.is_start && !right.is_start;
        return left.source_event_index < right.source_event_index;
    });

    for (auto fact : facts) {
        if (fact.is_start && fact.role != "prefetch_progress") continue;

        summary.processed_hicache_events++;
        summary.processed_events_by_role[fact.role]++;
        if (config_.page_size > 0 && fact.page_size > 0 && config_.page_size != fact.page_size) {
            const bool target_completion_evidence = contains(fact.role, "l3_to_l2") && !fact.target_pages.empty();
            if (!target_completion_evidence && is_non_invariant_observed_role(fact.role)) {
                summary.skipped_non_invariant_events++;
                continue;
            }
            if (!fact.target_pages.empty()) {
                fact.pages = fact.target_pages;
                // 旧版 page_hashes_concat 会把 prefix_keys + new_input_tokens 的
                // full path pages 全部写入 target_page_identity。prefetch_schedule
                // 真正需要的是 new_input_tokens 在 target page size 下产生的
                // suffix pages；按目标 page size 从尾部裁剪可兼容旧 trace。
                if (fact.role == "prefetch_schedule" && fact.new_input_tokens > 0 && config_.page_size > 0) {
                    const size_t suffix_pages = static_cast<size_t>(fact.new_input_tokens / config_.page_size);
                    if (suffix_pages > 0 && fact.pages.size() > suffix_pages)
                        fact.pages.erase(fact.pages.begin(), fact.pages.end() - static_cast<long>(suffix_pages));
                }
            }
            else if (fact.role == "prefetch_progress") {
                // `operation_hash_pages` 和 completed token 计数属于 base
                // prefetch operation，不随 target page size 保持页身份不变量。
                // 这里保留 check_return/ongoing/request_id，让 terminal progress
                // 仍能终止 pending prefetch，但不能凭 base 页创建 target ready/L2。
                fact.pages.clear();
                fact.prefetch_ready_page_count = 0;
            }
            else if (fact.requires_page_identity) {
                summary.missing_invariant_facts["target_page_identity_or_token_path"]++;
                continue;
            }
        }
        else if (should_skip_non_invariant_target_movement(config_, fact)) {
            summary.skipped_non_invariant_events++;
            continue;
        }
        if (fact.pages.empty() && fact.role != "insert" && fact.role != "evict_summary" && fact.role != "prefetch_progress") {
            if (fact.requires_page_identity) summary.missing_page_identity_events++;
            continue;
        }
        auto transitions = state_.apply_fact(fact, summary);
        summary.transition_trace.insert(summary.transition_trace.end(), transitions.begin(), transitions.end());
    }
    auto final_transitions = state_.finalize(summary);
    summary.transition_trace.insert(summary.transition_trace.end(), final_transitions.begin(), final_transitions.end());

    summary.l1_resident_pages = sorted_vector(state_.l1());
    summary.l2_resident_pages = sorted_vector(state_.l2());
    summary.l3_resident_pages = sorted_vector(state_.l3());
    summary.dirty_pages = sorted_vector(state_.dirty());
    summary.backuped_pages = sorted_vector(state_.backuped());
    summary.evicted_pages = sorted_vector(state_.evicted());
    summary.locked_pages = sorted_vector(state_.locked());
    summary.prefetch_planned_pages = sorted_vector(state_.prefetch_planned());
    summary.prefetch_ready_pages = sorted_vector(state_.prefetch_ready());
    summary.prefetch_late_pages = sorted_vector(state_.prefetch_late());
    summary.prefetch_suppressed_pages = sorted_vector(state_.prefetch_suppressed());
    summary.page_hit_counts = state_.hit_counts();

    if (summary.missing_page_identity_events > 0) summary.warnings.push_back("Some HiCache events cannot update state because page_identity is missing.");
    if (!summary.missing_invariant_facts.empty()) summary.warnings.push_back("Some HiCache target-state inputs are missing invariant facts.");
    if (summary.dirty_eviction_events > 0) summary.warnings.push_back("Dirty page eviction triggered modeled writeback state transitions.");
    summary.warnings.push_back("HiCacheModule maintains state only; no DAG mutations are applied.");
    return summary;
}

std::string HiCacheSummary::to_json() const {
    // summary 描述 HiCache 状态验证结果，不参与默认 E2E prediction。
    Json transition_rows = Json::array();
    for (const auto & transition : transition_trace) transition_rows.push_back(transition_to_json(transition, target_config.emit_state_digests));

    Json root;
    root["status"] = status;
    root["input_hicache_events"] = input_hicache_events;
    root["processed_hicache_events"] = processed_hicache_events;
    root["state_transition_count"] = state_transition_count;
    root["dag_mutations"] = dag_mutations;
    root["missing_page_identity_events"] = missing_page_identity_events;
    root["dirty_eviction_events"] = dirty_eviction_events;
    root["lock_state_events"] = lock_state_events;
    root["skipped_non_invariant_events"] = skipped_non_invariant_events;
    root["target_config"] = {
        {"page_size", target_config.page_size},
        {"l1_capacity_pages", target_config.l1_capacity_pages},
        {"l2_capacity_pages", target_config.l2_capacity_pages},
        {"write_policy", target_config.write_policy},
        {"write_through_threshold", target_config.write_through_threshold},
        {"prefetch_policy", target_config.prefetch_policy},
        {"prefetch_timeout_configured", target_config.prefetch_timeout_configured},
        {"prefetch_timeout_base_sec", target_config.prefetch_timeout_base_sec},
        {"prefetch_timeout_per_ki_token_sec", target_config.prefetch_timeout_per_ki_token_sec},
        {"prefetch_timeout_max_sec", target_config.prefetch_timeout_max_sec},
        {"write_back_prefetch_transfer_credit", target_config.write_back_prefetch_transfer_credit},
        {"emit_state_digests", target_config.emit_state_digests},
    };
    root["events_by_role"] = events_by_role;
    root["processed_events_by_role"] = processed_events_by_role;
    root["transitions_by_kind"] = transitions_by_kind;
    root["missing_invariant_facts"] = missing_invariant_facts;
    root["transition_trace"] = transition_rows;
    root["final_state"] = {
        {"l1_resident_pages", l1_resident_pages},
        {"l2_resident_pages", l2_resident_pages},
        {"l3_resident_pages", l3_resident_pages},
        {"dirty_pages", dirty_pages},
        {"backuped_pages", backuped_pages},
        {"evicted_pages", evicted_pages},
        {"prefetch_planned_pages", prefetch_planned_pages},
        {"prefetch_ready_pages", prefetch_ready_pages},
        {"prefetch_late_pages", prefetch_late_pages},
        {"prefetch_suppressed_pages", prefetch_suppressed_pages},
        {"page_hit_counts", page_hit_counts},
        {"counts",
         {
             {"l1_resident_pages", l1_resident_pages.size()},
             {"l2_resident_pages", l2_resident_pages.size()},
             {"l3_resident_pages", l3_resident_pages.size()},
             {"dirty_pages", dirty_pages.size()},
             {"backuped_pages", backuped_pages.size()},
             {"evicted_pages", evicted_pages.size()},
             {"prefetch_planned_pages", prefetch_planned_pages.size()},
             {"prefetch_ready_pages", prefetch_ready_pages.size()},
             {"prefetch_late_pages", prefetch_late_pages.size()},
             {"prefetch_suppressed_pages", prefetch_suppressed_pages.size()},
             {"page_hit_counts", page_hit_counts.size()},
         }},
    };
    if (lock_state_events > 0 || !locked_pages.empty()) {
        root["final_state"]["locked_pages"] = locked_pages;
        root["final_state"]["counts"]["locked_pages"] = locked_pages.size();
    }
    root["warnings"] = warnings;
    return root.dump();
}

HiCacheSummary apply_hicache_model(DagGraph & graph, const HiCacheConfig & config) {
    HiCacheStateModel model(config);
    return model.run(graph);
}

} // namespace TraceGraph
