#include "trace_graph/modules/hicache/hicache_model.hpp"

#include "trace_graph/core/dag_graph.hpp"
#include "trace_graph/core/trace_event.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <unordered_set>

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

std::string unescape_json_quote_markers(const std::string & value) {
    // TraceScanner 当前不完整反转义 JSON string。HiCache page_identity 经常是 list 被写成字符串，
    // 这里只处理 \" 和 \\ 两类会污染 page key 的最小转义。
    std::string result;
    result.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size() && (value[i + 1] == '"' || value[i + 1] == '\\')) {
            result.push_back(value[i + 1]);
            ++i;
        }
        else { result.push_back(value[i]); }
    }
    return result;
}

std::string arg_string(const TraceEvent & event, const std::string & key) {
    auto it = event.args.find(key);
    return it == event.args.end() ? "" : it->second;
}

bool is_hicache_event(const TraceEvent & event) {
    // 状态建模阶段仍使用偏宽识别条件，目的是确认 profiling 是否已经把 HiCache 事实送进 C++ 后端。
    if (event.cat == "hicache") return true;
    if (starts_with(event.name, "HiCache::")) return true;
    if (starts_with(event.name, "hicache_")) return true;
    auto domain = arg_string(event, "domain");
    return domain == "hicache" || (domain == "python_probe" && contains(event.name, "hicache"));
}

std::string normalize_tier(const std::string & value) {
    auto tier = upper(trim(value));
    if (tier == "DEVICE" || tier == "L1") return "L1";
    if (tier == "HOST" || tier == "L2") return "L2";
    if (tier == "STORAGE" || tier == "L3") return "L3";
    return "";
}

bool bool_arg(const TraceEvent & event, const std::string & key, bool fallback) {
    auto value = lower(trim(event.arg(key)));
    if (value.empty()) return fallback;
    if (value == "true" || value == "1" || value == "yes") return true;
    if (value == "false" || value == "0" || value == "no") return false;
    return fallback;
}

std::string role_from_name(const TraceEvent & event) {
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
    if (contains(name, "evict")) return "evict";
    if (contains(name, "lookup") || contains(name, "match_prefix")) return "lookup";
    return name.empty() ? "unknown" : name;
}

std::string infer_role(const TraceEvent & event) {
    auto role = lower(trim(event.arg("event_role")));
    if (!role.empty()) return role;

    auto direction = lower(trim(event.arg("direction")));
    auto src = normalize_tier(event.arg("tier_src"));
    auto dst = normalize_tier(event.arg("tier_dst"));
    if (!src.empty() && !dst.empty() && !direction.empty()) return lower(src) + "_to_" + lower(dst) + "_" + direction;
    if (!direction.empty()) return direction;
    return role_from_name(event);
}

bool is_start_fact(const TraceEvent & event) { return ends_with(event.name, "_start"); }

std::vector<std::string> pages_from_json_array(const std::string & raw) {
    std::vector<std::string> pages;
    try {
        auto value = Json::parse(raw);
        if (value.is_array()) {
            for (const auto & item : value) {
                if (item.is_null()) continue;
                if (item.is_string()) pages.push_back(trim(item.get<std::string>()));
                else if (item.is_number_unsigned()) pages.push_back(std::to_string(item.get<uint64_t>()));
                else if (item.is_number_integer()) pages.push_back(std::to_string(item.get<int64_t>()));
                else pages.push_back(item.dump());
            }
        }
        else if (value.is_string()) pages.push_back(trim(value.get<std::string>()));
    }
    catch (...) {
    }
    return pages;
}

std::vector<std::string> parse_page_identity(const TraceEvent & event) {
    auto raw = unescape_json_quote_markers(trim(event.arg("page_identity")));
    if (raw.empty()) raw = unescape_json_quote_markers(trim(event.arg("hash_value")));
    if (raw.empty()) return {};

    auto pages = pages_from_json_array(raw);
    if (pages.empty()) {
        std::string normalized = raw;
        for (char & ch : normalized) {
            if (ch == '[' || ch == ']' || ch == '"' || ch == '\'' || ch == '|' || ch == ',' || ch == ';' || std::isspace(static_cast<unsigned char>(ch))) ch = ' ';
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

bool role_requires_page_identity(const std::string & role, const TraceEvent & event) {
    if (ends_with(role, "_start") || contains(role, "progress") || role == "prefetch_loaded_tokens" || role == "prefetch_decision") return false;
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
    };
    if (exact_roles.count(role)) return true;
    if (contains(role, "to_l1") || contains(role, "to_l2") || contains(role, "to_l3")) return true;
    if (event.arg_u64("num_pages", 0) > 0 || event.arg_u64("hash_pages", 0) > 0) return true;
    return false;
}

struct HiCacheState {
    std::set<std::string> l1;
    std::set<std::string> l2;
    std::set<std::string> l3;
    std::set<std::string> dirty;
    std::set<std::string> backuped;
    std::set<std::string> evicted;
    std::set<std::string> prefetch_planned;
    std::set<std::string> prefetch_ready;
};

std::set<std::string> * tier_set(HiCacheState & state, const std::string & tier) {
    if (tier == "L1") return &state.l1;
    if (tier == "L2") return &state.l2;
    if (tier == "L3") return &state.l3;
    return nullptr;
}

std::vector<std::string> sorted_vector(const std::set<std::string> & values) { return {values.begin(), values.end()}; }

void record_transition(HiCacheSummary & summary, const std::string & kind) {
    summary.state_transition_count++;
    summary.transitions_by_kind[kind]++;
}

void add_resident(HiCacheState & state, HiCacheSummary & summary, const std::string & tier, const std::string & page) {
    auto * pages = tier_set(state, tier);
    if (!pages) return;
    if (pages->insert(page).second) record_transition(summary, "add_" + lower(tier) + "_resident");
    state.evicted.erase(page);
}

void remove_resident(HiCacheState & state, HiCacheSummary & summary, const std::string & tier, const std::string & page) {
    auto * pages = tier_set(state, tier);
    if (!pages) return;
    if (pages->erase(page) > 0) record_transition(summary, "remove_" + lower(tier) + "_resident");
}

void mark_dirty(HiCacheState & state, HiCacheSummary & summary, const std::string & page) {
    if (state.dirty.insert(page).second) record_transition(summary, "mark_dirty");
}

void clear_dirty(HiCacheState & state, HiCacheSummary & summary, const std::string & page) {
    if (state.dirty.erase(page) > 0) record_transition(summary, "clear_dirty");
}

void mark_backuped(HiCacheState & state, HiCacheSummary & summary, const std::string & page) {
    if (state.backuped.insert(page).second) record_transition(summary, "mark_backuped");
}

void mark_prefetch_planned(HiCacheState & state, HiCacheSummary & summary, const std::string & page) {
    if (state.prefetch_planned.insert(page).second) record_transition(summary, "mark_prefetch_planned");
}

void mark_prefetch_ready(HiCacheState & state, HiCacheSummary & summary, const std::string & page) {
    if (state.prefetch_ready.insert(page).second) record_transition(summary, "mark_prefetch_ready");
}

void apply_load_to_l1(HiCacheState & state, HiCacheSummary & summary, const TraceEvent & event, const std::vector<std::string> & pages) {
    auto src = normalize_tier(event.arg("tier_src"));
    if (src.empty()) src = "L2";
    for (const auto & page : pages) {
        add_resident(state, summary, src, page);
        add_resident(state, summary, "L1", page);
    }
}

void apply_l3_to_l2(HiCacheState & state, HiCacheSummary & summary, const std::vector<std::string> & pages, bool prefetch_ready) {
    for (const auto & page : pages) {
        add_resident(state, summary, "L3", page);
        add_resident(state, summary, "L2", page);
        if (prefetch_ready) mark_prefetch_ready(state, summary, page);
    }
}

void apply_write_to_l2(HiCacheState & state, HiCacheSummary & summary, const std::vector<std::string> & pages) {
    for (const auto & page : pages) {
        add_resident(state, summary, "L1", page);
        add_resident(state, summary, "L2", page);
        mark_backuped(state, summary, page);
        clear_dirty(state, summary, page);
    }
}

void apply_write_to_l3(HiCacheState & state, HiCacheSummary & summary, const std::vector<std::string> & pages) {
    for (const auto & page : pages) {
        add_resident(state, summary, "L2", page);
        add_resident(state, summary, "L3", page);
        mark_backuped(state, summary, page);
        clear_dirty(state, summary, page);
    }
}

void apply_insert(HiCacheState & state, HiCacheSummary & summary, const TraceEvent & event, const std::vector<std::string> & pages) {
    auto dst = normalize_tier(event.arg("tier_dst"));
    if (dst.empty()) dst = "L1";
    auto dirty = bool_arg(event, "dirty", true);
    auto backuped = bool_arg(event, "backuped", false);
    for (const auto & page : pages) {
        add_resident(state, summary, dst, page);
        if (dirty) mark_dirty(state, summary, page);
        if (backuped) mark_backuped(state, summary, page);
    }
}

void apply_evict(HiCacheState & state, HiCacheSummary & summary, const TraceEvent & event, const std::vector<std::string> & pages) {
    auto src = normalize_tier(event.arg("tier_src"));
    for (const auto & page : pages) {
        if (state.dirty.count(page)) summary.dirty_eviction_events++;
        if (!src.empty()) remove_resident(state, summary, src, page);
        else if (state.l1.count(page)) remove_resident(state, summary, "L1", page);
        else if (state.l2.count(page)) remove_resident(state, summary, "L2", page);
        else if (state.l3.count(page)) remove_resident(state, summary, "L3", page);
        if (state.evicted.insert(page).second) record_transition(summary, "mark_evicted");
    }
}

void apply_generic_tier_move(HiCacheState & state, HiCacheSummary & summary, const TraceEvent & event, const std::vector<std::string> & pages) {
    auto src = normalize_tier(event.arg("tier_src"));
    auto dst = normalize_tier(event.arg("tier_dst"));
    auto direction = lower(trim(event.arg("direction")));
    for (const auto & page : pages) {
        if (!src.empty()) add_resident(state, summary, src, page);
        if (!dst.empty()) add_resident(state, summary, dst, page);
        if (direction == "write" || dst == "L3") {
            mark_backuped(state, summary, page);
            clear_dirty(state, summary, page);
        }
        if (direction == "evict" && !src.empty()) remove_resident(state, summary, src, page);
    }
}

void apply_role_event(HiCacheState & state, HiCacheSummary & summary, const TraceEvent & event, const std::string & role, const std::vector<std::string> & pages) {
    if (role == "insert") {
        apply_insert(state, summary, event, pages);
        return;
    }
    if (role == "load_back" || role == "init_load_back" || contains(role, "l2_to_l1") || normalize_tier(event.arg("tier_dst")) == "L1") {
        apply_load_to_l1(state, summary, event, pages);
        return;
    }
    if (role == "prefetch_schedule" || role == "l3_prefetch_enqueue") {
        for (const auto & page : pages) mark_prefetch_planned(state, summary, page);
        return;
    }
    if (role == "l3_hit_query") {
        for (const auto & page : pages) add_resident(state, summary, "L3", page);
        return;
    }
    if (role == "l3_to_l2_transfer" || contains(role, "l3_to_l2") || (normalize_tier(event.arg("tier_src")) == "L3" && normalize_tier(event.arg("tier_dst")) == "L2")) {
        apply_l3_to_l2(state, summary, pages, contains(role, "prefetch") || event.arg("direction") == "prefetch");
        return;
    }
    if (role == "write_backup" || contains(role, "l1_to_l2") || (normalize_tier(event.arg("tier_src")) == "L1" && normalize_tier(event.arg("tier_dst")) == "L2")) {
        apply_write_to_l2(state, summary, pages);
        return;
    }
    if (role == "write_storage_schedule" || contains(role, "l2_to_l3")
        || (normalize_tier(event.arg("tier_src")) == "L2" && normalize_tier(event.arg("tier_dst")) == "L3")) {
        apply_write_to_l3(state, summary, pages);
        return;
    }
    if (role == "evict") {
        apply_evict(state, summary, event, pages);
        return;
    }
    apply_generic_tier_move(state, summary, event, pages);
}

} // namespace

std::string HiCacheSummary::to_json() const {
    // summary 描述 HiCache 状态验证结果，不参与默认 E2E prediction。
    Json root;
    root["status"] = status;
    root["input_hicache_events"] = input_hicache_events;
    root["processed_hicache_events"] = processed_hicache_events;
    root["state_transition_count"] = state_transition_count;
    root["dag_mutations"] = dag_mutations;
    root["missing_page_identity_events"] = missing_page_identity_events;
    root["dirty_eviction_events"] = dirty_eviction_events;
    root["events_by_role"] = events_by_role;
    root["processed_events_by_role"] = processed_events_by_role;
    root["transitions_by_kind"] = transitions_by_kind;
    root["final_state"] = {
        {"l1_resident_pages", l1_resident_pages},
        {"l2_resident_pages", l2_resident_pages},
        {"l3_resident_pages", l3_resident_pages},
        {"dirty_pages", dirty_pages},
        {"backuped_pages", backuped_pages},
        {"evicted_pages", evicted_pages},
        {"prefetch_planned_pages", prefetch_planned_pages},
        {"prefetch_ready_pages", prefetch_ready_pages},
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
         }},
    };
    root["warnings"] = warnings;
    return root.dump();
}

HiCacheSummary apply_hicache_model(DagGraph & graph, const HiCacheConfig & config) {
    HiCacheSummary summary;
    if (!config.enabled) {
        summary.status = "disabled";
        return summary;
    }

    HiCacheState state;

    // 当前 HiCache 模块只维护状态，不修改 DAG。
    // 这样先验证 cache state faithful replay，不让未完成的 patch 逻辑干扰 base DAG。
    for (const auto & node : graph.nodes()) {
        const auto & event = graph.event_for_node(node.id);
        if (!is_hicache_event(event)) continue;

        summary.input_hicache_events++;
        auto role = infer_role(event);
        if (role.empty()) role = "unknown";
        summary.events_by_role[role]++;
        if (is_start_fact(event)) continue;

        summary.processed_hicache_events++;
        summary.processed_events_by_role[role]++;
        auto pages = parse_page_identity(event);
        if (pages.empty()) {
            if (role_requires_page_identity(role, event)) summary.missing_page_identity_events++;
            continue;
        }
        apply_role_event(state, summary, event, role, pages);
    }

    summary.l1_resident_pages = sorted_vector(state.l1);
    summary.l2_resident_pages = sorted_vector(state.l2);
    summary.l3_resident_pages = sorted_vector(state.l3);
    summary.dirty_pages = sorted_vector(state.dirty);
    summary.backuped_pages = sorted_vector(state.backuped);
    summary.evicted_pages = sorted_vector(state.evicted);
    summary.prefetch_planned_pages = sorted_vector(state.prefetch_planned);
    summary.prefetch_ready_pages = sorted_vector(state.prefetch_ready);

    if (summary.missing_page_identity_events > 0) summary.warnings.push_back("Some HiCache events cannot update state because page_identity is missing.");
    if (summary.dirty_eviction_events > 0) summary.warnings.push_back("Dirty pages were evicted without an observed writeback before eviction.");
    summary.warnings.push_back("HiCacheModule maintains state only; no DAG mutations are applied.");
    return summary;
}

} // namespace TraceGraph
