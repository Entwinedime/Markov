#include "trace_graph/domains/cache_io/cache_model.hpp"

#include "trace_graph/activity_record.hpp"
#include "trace_graph/logger.hpp"
#include "trace_graph/trace_dag.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace TraceGraph {

namespace {

bool starts_with(const std::string & text, const std::string & prefix) { return text.rfind(prefix, 0) == 0; }

bool contains(const std::string & text, const std::string & needle) { return text.find(needle) != std::string::npos; }

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string arg_string(const ActivityRecord & record, const std::string & key, const std::string & def = "") {
    auto it = record.args.find(key);
    return it == record.args.end() ? def : it->second;
}

uint64_t arg_u64(const ActivityRecord & record, const std::string & key, uint64_t def = 0) {
    auto it = record.args.find(key);
    if (it == record.args.end() || it->second.empty()) return def;
    try {
        double value = std::stod(it->second);
        if (value < 0) return def;
        return static_cast<uint64_t>(value);
    }
    catch (...) {
        return def;
    }
}

bool has_positive_arg(const ActivityRecord & record, const std::string & key) { return arg_u64(record, key, 0) > 0; }

bool is_hicache_event(const ActivityRecord & record) {
    if (record.cat == "hicache") return true;
    if (starts_with(record.name, "HiCache::")) return true;
    return arg_string(record, "domain") == "cache_io";
}

std::string infer_event_kind(const ActivityRecord & record) {
    auto explicit_kind = lower(arg_string(record, "event_kind"));
    if (!explicit_kind.empty()) return explicit_kind;

    auto direction = lower(arg_string(record, "direction"));
    if (direction == "backup" || direction == "write" || direction == "load" || direction == "prefetch" || direction == "insert"
        || direction == "evict" || direction == "release" || direction == "transfer")
        return "movement";

    if (contains(record.name, "transfer") || contains(record.name, "load_l2_to_l1") || contains(record.name, "l3_to_l2")
        || contains(record.name, "l1_to_l2") || contains(record.name, "l2_to_l3") || contains(record.name, "evict")
        || contains(record.name, "insert"))
        return "movement";

    return "control";
}

std::string infer_src(const ActivityRecord & record) {
    auto explicit_src = arg_string(record, "tier_src");
    if (!explicit_src.empty()) return explicit_src;
    if (contains(record.name, "l3_to_l2")) return "L3";
    if (contains(record.name, "l2_to_l1")) return "L2";
    if (contains(record.name, "l1_to_l2")) return "L1";
    if (contains(record.name, "l2_to_l3")) return "L2";
    if (contains(record.name, "evict_l1")) return "L1";
    if (contains(record.name, "evict_l2")) return "L2";
    return "";
}

std::string infer_dst(const ActivityRecord & record) {
    auto explicit_dst = arg_string(record, "tier_dst");
    if (!explicit_dst.empty()) return explicit_dst;
    if (contains(record.name, "l3_to_l2")) return "L2";
    if (contains(record.name, "l2_to_l1")) return "L1";
    if (contains(record.name, "l1_to_l2")) return "L2";
    if (contains(record.name, "l2_to_l3")) return "L3";
    return "";
}

uint64_t parse_config_u64(const std::string & value, uint64_t def) {
    if (value.empty() || value == "infer" || value == "infinite" || value == "inf") return def;
    try {
        double numeric = std::stod(value);
        if (numeric < 0) return def;
        return static_cast<uint64_t>(numeric);
    }
    catch (...) {
        return def;
    }
}

bool use_scenario_page_size(const CacheIOConfig & config) {
    auto policy = lower(config.page_size_policy);
    return policy == "scenario" || policy == "config" || policy == "override";
}

uint64_t config_page_size(const CacheIOConfig & config) { return parse_config_u64(config.page_size_tokens, 0); }

uint64_t infer_bytes_per_page(const ActivityRecord & record, const CacheIOConfig & config) {
    uint64_t bytes_per_page = parse_config_u64(config.bytes_per_page, 0);
    if (bytes_per_page > 0) return bytes_per_page;

    if (!use_scenario_page_size(config)) {
        bytes_per_page = arg_u64(record, "bytes_per_page", 0);
        if (bytes_per_page > 0) return bytes_per_page;
    }

    uint64_t page_size = use_scenario_page_size(config) ? config_page_size(config) : 0;
    if (page_size == 0) page_size = arg_u64(record, "page_size", 0);
    if (page_size == 0) page_size = config_page_size(config);
    if (page_size == 0 || config.num_layers == 0 || config.num_kv_heads == 0 || config.head_dim == 0 || config.dtype_bytes == 0) return 0;

    uint64_t tp_size = std::max<uint64_t>(1, config.tp_size);
    uint64_t kv_heads_per_rank = (config.num_kv_heads + tp_size - 1) / tp_size;
    return page_size * config.num_layers * kv_heads_per_rank * config.head_dim * 2 * config.dtype_bytes;
}

uint64_t infer_bytes(const ActivityRecord & record, const CacheIOConfig & config, uint64_t pages) {
    uint64_t bytes = arg_u64(record, "bytes", 0);
    if (bytes > 0 && !use_scenario_page_size(config)) return bytes;

    uint64_t bytes_per_page = infer_bytes_per_page(record, config);
    if (pages > 0 && bytes_per_page > 0) return pages * bytes_per_page;
    if (bytes > 0) return bytes;
    return 0;
}

uint64_t infer_pages(const ActivityRecord & record, const CacheIOConfig & config) {
    uint64_t scenario_page_size = use_scenario_page_size(config) ? config_page_size(config) : 0;
    if (scenario_page_size > 0) {
        uint64_t tokens = arg_u64(record, "num_tokens", 0);
        if (tokens > 0) return (tokens + scenario_page_size - 1) / scenario_page_size;

        uint64_t trace_pages = arg_u64(record, "num_pages", 0);
        uint64_t trace_page_size = arg_u64(record, "page_size", 0);
        if (trace_pages > 0 && trace_page_size > 0) {
            uint64_t trace_tokens = trace_pages * trace_page_size;
            return (trace_tokens + scenario_page_size - 1) / scenario_page_size;
        }
    }

    uint64_t pages = arg_u64(record, "num_pages", 0);
    if (pages > 0) return pages;

    uint64_t tokens = arg_u64(record, "num_tokens", 0);
    uint64_t page_size = arg_u64(record, "page_size", 0);
    if (page_size == 0) page_size = config_page_size(config);
    if (tokens > 0 && page_size > 0) return (tokens + page_size - 1) / page_size;
    return 0;
}

const CacheIOTierConfig * find_tier(const CacheIOConfig & config, const std::string & name) {
    for (const auto & tier : config.tiers) {
        if (tier.name == name) return &tier;
    }
    return nullptr;
}

double tier_latency(const CacheIOConfig & config, const std::string & name) {
    auto tier = find_tier(config, name);
    return tier ? tier->latency_us : 0.0;
}

double tier_bandwidth(const CacheIOConfig & config, const std::string & name) {
    auto tier = find_tier(config, name);
    if (!tier || tier->bandwidth_infer || tier->bandwidth_infinite || tier->bandwidth_gbps <= 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    return tier->bandwidth_gbps;
}

uint64_t estimate_transfer_us(const CacheIOConfig & config, const std::string & src, const std::string & dst, uint64_t bytes, uint64_t observed) {
    double latency = std::max(tier_latency(config, src), tier_latency(config, dst));
    double bandwidth = std::min(tier_bandwidth(config, src), tier_bandwidth(config, dst));
    double transfer = 0.0;
    if (std::isfinite(bandwidth) && bandwidth > 0.0 && bytes > 0) {
        transfer = static_cast<double>(bytes) / (bandwidth * 1000.0);
    }
    auto estimated = static_cast<uint64_t>(std::ceil(latency + transfer));
    if (estimated == 0) return observed;
    return estimated;
}

bool is_eviction(const ActivityRecord & record) {
    auto direction = arg_string(record, "direction");
    return direction == "evict" || contains(record.name, "evict");
}

bool is_writeback(const ActivityRecord & record) {
    auto direction = arg_string(record, "direction");
    return direction == "backup" || direction == "write" || contains(record.name, "backup") || contains(record.name, "write_l2_to_l3");
}

bool is_transfer(const std::string & src, const std::string & dst) { return !src.empty() && !dst.empty() && src != dst; }

bool is_python_probe_event(const ActivityRecord & record) {
    return arg_string(record, "producer") == "python_probe"
        && (!arg_string(record, "python_method").empty() || !arg_string(record, "python_function").empty());
}

bool is_physical_transfer_event(const ActivityRecord & record) {
    if (!is_python_probe_event(record)) return infer_event_kind(record) == "movement";

    auto function = arg_string(record, "python_function");
    if (!function.empty()) return true;

    auto method = arg_string(record, "python_method");
    return method == "start_writing" || method == "start_loading" || method == "_generic_page_set" || method == "_page_set_zero_copy"
        || method == "_generic_page_get" || method == "_page_get_zero_copy" || method == "_draft_page_set" || method == "_draft_page_get";
}

bool is_state_movement_event(const ActivityRecord & record) {
    auto direction = lower(arg_string(record, "direction"));
    if (direction == "insert" || direction == "evict" || direction == "release") return true;
    if (!is_python_probe_event(record)) return infer_event_kind(record) == "movement" && !is_transfer(infer_src(record), infer_dst(record));

    auto method = arg_string(record, "python_method");
    return method == "insert" || method == "evict_device" || method == "evict_host" || method == "_evict_backuped" || method == "_evict_regular"
        || method == "append_host_mem_release" || method == "_append_host_mem_release_pages";
}

std::vector<std::string> split_keys(const std::string & encoded) {
    std::vector<std::string> keys;
    std::string token;
    char delimiter = encoded.find('|') != std::string::npos ? '|' : ',';
    std::istringstream stream(encoded);
    while (std::getline(stream, token, delimiter)) {
        if (!token.empty()) keys.push_back(token);
    }
    return keys;
}

std::vector<std::string> adjust_keys_for_pages(const std::vector<std::string> & keys, uint64_t pages) {
    if (keys.empty() || pages == 0 || keys.size() == pages) return keys;
    std::vector<std::string> adjusted;
    adjusted.reserve(static_cast<size_t>(pages));
    if (pages > keys.size()) {
        uint64_t base_repeat = pages / keys.size();
        uint64_t remainder = pages % keys.size();
        for (size_t i = 0; i < keys.size(); ++i) {
            uint64_t repeat = base_repeat + (i < remainder ? 1 : 0);
            for (uint64_t j = 0; j < repeat; ++j) { adjusted.push_back(keys[i] + "#" + std::to_string(j)); }
        }
    }
    else {
        for (uint64_t i = 0; i < pages; ++i) {
            uint64_t begin = i * keys.size() / pages;
            uint64_t end = (i + 1) * keys.size() / pages;
            if (end <= begin) end = begin + 1;
            std::ostringstream os;
            for (uint64_t j = begin; j < end && j < keys.size(); ++j) {
                if (j > begin) os << "+";
                os << keys[static_cast<size_t>(j)];
            }
            adjusted.push_back(os.str());
        }
    }
    return adjusted;
}

void append_warning(CacheIOSummary & summary, const std::string & warning) {
    if (std::find(summary.whatif_warnings.begin(), summary.whatif_warnings.end(), warning) == summary.whatif_warnings.end()) {
        summary.whatif_warnings.push_back(warning);
    }
}

bool status_completed(const ActivityRecord & record) {
    auto status = lower(arg_string(record, "status"));
    return status.empty() || status == "ok" || status == "done" || status == "completed" || status == "success";
}

bool prefetch_event_is_enabled(const CacheIOConfig & config, const ActivityRecord & record, uint64_t pages) {
    auto policy = lower(config.prefetch_policy);
    if (policy == "none") return false;
    if (config.prefetch_threshold > 0) {
        uint64_t page_size = arg_u64(record, "page_size", 0);
        if (page_size == 0) page_size = config_page_size(config);
        uint64_t tokens = arg_u64(record, "num_tokens", 0);
        if (tokens == 0 && page_size > 0) tokens = pages * page_size;
        if (tokens > 0 && tokens < config.prefetch_threshold) return false;
    }
    if (policy == "best_effort") return status_completed(record);
    return true;
}

uint64_t timeout_cap_us(const CacheIOConfig & config, const ActivityRecord & record, uint64_t pages) {
    double cap = config.prefetch_timeout_base;
    uint64_t tokens = arg_u64(record, "num_tokens", 0);
    if (tokens == 0) {
        uint64_t page_size = arg_u64(record, "page_size", 0);
        if (page_size == 0) page_size = config_page_size(config);
        tokens = pages * page_size;
    }
    cap += (static_cast<double>(tokens) / 1024.0) * config.prefetch_timeout_per_ki_token;
    if (config.prefetch_timeout_max > 0.0) cap = std::min(cap, config.prefetch_timeout_max);
    if (cap <= 0.0) return 0;
    return static_cast<uint64_t>(std::ceil(cap * 1000000.0));
}

bool should_count_transfer_foreground(const CacheIOConfig & config,
                                      const ActivityRecord & record,
                                      const std::string & src,
                                      const std::string & dst,
                                      uint64_t pages,
                                      uint64_t estimated) {
    auto direction = lower(arg_string(record, "direction"));
    auto policy = lower(config.prefetch_policy);
    if (src == "L3" && dst == "L2" && direction == "prefetch") {
        if (config.count_async_prefetch_latency) return true;
        if (policy == "wait_complete") return true;
        if (policy == "timeout") return timeout_cap_us(config, record, pages) > 0 && estimated > 0;
        return false;
    }
    return true;
}

uint64_t foreground_estimate_for_transfer(const CacheIOConfig & config,
                                          const ActivityRecord & record,
                                          const std::string & src,
                                          const std::string & dst,
                                          uint64_t bytes,
                                          uint64_t pages,
                                          uint64_t observed) {
    uint64_t estimated = estimate_transfer_us(config, src, dst, bytes, observed);
    if (src == "L3" && dst == "L2" && lower(config.prefetch_policy) == "timeout" && lower(arg_string(record, "direction")) == "prefetch") {
        uint64_t cap = timeout_cap_us(config, record, pages);
        if (cap > 0) return std::min(estimated, cap);
    }
    return estimated;
}

struct TierRuntimeState {
    std::unordered_set<std::string> resident;
    std::unordered_set<std::string> dirty;
    std::deque<std::string> order;
    uint64_t anonymous_pages = 0;
    uint64_t dirty_anonymous_pages = 0;
};

using RuntimeTiers = std::unordered_map<std::string, TierRuntimeState>;

bool has_page(RuntimeTiers & tiers, const std::string & tier, const std::string & key) {
    auto it = tiers.find(tier);
    return it != tiers.end() && it->second.resident.find(key) != it->second.resident.end();
}

void remove_from_order(TierRuntimeState & state, const std::string & key) {
    auto it = std::find(state.order.begin(), state.order.end(), key);
    if (it != state.order.end()) state.order.erase(it);
}

void touch_page(RuntimeTiers & tiers, const CacheIOConfig & config, const std::string & tier_name, const std::string & key, bool dirty = false) {
    if (tier_name.empty() || key.empty()) return;
    auto & state = tiers[tier_name];
    auto tier = find_tier(config, tier_name);
    bool existed = state.resident.find(key) != state.resident.end();
    state.resident.insert(key);
    if (dirty) state.dirty.insert(key);

    if (tier && lower(tier->eviction) == "fifo" && existed) return;
    remove_from_order(state, key);
    state.order.push_back(key);
}

void remove_page(RuntimeTiers & tiers, const std::string & tier_name, const std::string & key) {
    auto it = tiers.find(tier_name);
    if (it == tiers.end()) return;
    it->second.resident.erase(key);
    it->second.dirty.erase(key);
    remove_from_order(it->second, key);
}

void mark_clean(RuntimeTiers & tiers, const std::string & tier_name, const std::string & key) {
    auto it = tiers.find(tier_name);
    if (it == tiers.end()) return;
    it->second.dirty.erase(key);
}

void add_anonymous_pages(RuntimeTiers & tiers, const std::string & tier_name, uint64_t pages, bool dirty = false) {
    if (tier_name.empty() || pages == 0) return;
    tiers[tier_name].anonymous_pages += pages;
    if (dirty) tiers[tier_name].dirty_anonymous_pages += pages;
}

void remove_anonymous_pages(RuntimeTiers & tiers, const std::string & tier_name, uint64_t pages) {
    auto it = tiers.find(tier_name);
    if (it == tiers.end() || pages == 0) return;
    it->second.anonymous_pages = it->second.anonymous_pages > pages ? it->second.anonymous_pages - pages : 0;
    it->second.dirty_anonymous_pages = it->second.dirty_anonymous_pages > pages ? it->second.dirty_anonymous_pages - pages : 0;
}

uint64_t resident_count(const RuntimeTiers & tiers, const std::string & tier_name) {
    auto it = tiers.find(tier_name);
    if (it == tiers.end()) return 0;
    return static_cast<uint64_t>(it->second.resident.size()) + it->second.anonymous_pages;
}

void add_edge_cost(CacheIOSummary & summary,
                   const CacheIOConfig & config,
                   const std::string & src,
                   const std::string & dst,
                   uint64_t pages,
                   uint64_t bytes,
                   uint64_t observed = 0,
                   bool count_latency = true,
                   bool background = false,
                   uint64_t latency_override = std::numeric_limits<uint64_t>::max());

bool write_back_policy(const CacheIOConfig & config) {
    auto policy = lower(config.write_policy);
    return policy == "write_back" || policy == "writeback";
}

void enforce_capacity(CacheIOSummary & summary,
                      RuntimeTiers & tiers,
                      const CacheIOConfig & config,
                      const std::string & tier_name,
                      uint64_t bytes_per_page = 0,
                      bool foreground_writeback = true) {
    auto tier = find_tier(config, tier_name);
    if (!tier || tier->capacity_infer || tier->capacity_infinite) return;
    auto & state = tiers[tier_name];
    while (resident_count(tiers, tier_name) > tier->capacity_pages) {
        if (state.resident.empty() && state.anonymous_pages == 0) break;

        if (lower(tier->eviction) == "infinite") break;
        bool dirty_victim = false;
        if (!state.order.empty()) {
            std::string victim = state.order.front();
            state.order.pop_front();
            dirty_victim = state.dirty.erase(victim) > 0;
            state.resident.erase(victim);
        }
        else if (state.anonymous_pages > 0) {
            state.anonymous_pages--;
            if (state.dirty_anonymous_pages > 0) {
                state.dirty_anonymous_pages--;
                dirty_victim = true;
            }
        }
        else {
            auto it = state.resident.begin();
            dirty_victim = state.dirty.erase(*it) > 0;
            state.resident.erase(it);
        }
        summary.eviction_events++;
        summary.evictions_by_tier[tier_name]++;
        if (dirty_victim && tier_name == "L2" && write_back_policy(config) && config.writeback_on_evict) {
            uint64_t bytes = bytes_per_page > 0 ? bytes_per_page : 0;
            add_edge_cost(summary, config, "L2", "L3", 1, bytes, 0, true, !foreground_writeback);
            summary.writeback_events++;
            summary.writebacks_by_edge["L2->L3"]++;
            summary.model_generated_movements++;
        }
    }
    summary.resident_pages_by_tier[tier_name] = static_cast<int64_t>(resident_count(tiers, tier_name));
}

uint64_t edge_bytes_for_pages(uint64_t event_bytes, uint64_t event_pages, uint64_t pages, uint64_t bytes_per_page) {
    if (pages == 0) return 0;
    if (bytes_per_page > 0) return pages * bytes_per_page;
    if (event_pages > 0 && event_bytes > 0) return static_cast<uint64_t>(std::ceil(static_cast<double>(event_bytes) * pages / event_pages));
    return event_bytes;
}

void add_edge_cost(CacheIOSummary & summary,
                   const CacheIOConfig & config,
                   const std::string & src,
                   const std::string & dst,
                   uint64_t pages,
                   uint64_t bytes,
                   uint64_t observed,
                   bool count_latency,
                   bool background,
                   uint64_t latency_override) {
    if (!is_transfer(src, dst)) return;
    std::string edge = src + "->" + dst;
    summary.pages_by_edge[edge] += pages;
    summary.bytes_by_edge[edge] += bytes;
    auto latency = latency_override == std::numeric_limits<uint64_t>::max() ? estimate_transfer_us(config, src, dst, bytes, observed) : latency_override;
    if (background) summary.background_cache_io_us += latency;
    else if (count_latency) {
        summary.foreground_cache_io_us += latency;
        summary.estimated_latency_us += latency;
    }
}

void update_resident_summary(CacheIOSummary & summary, const RuntimeTiers & tiers) {
    for (const auto & kv : tiers) { summary.resident_pages_by_tier[kv.first] = static_cast<int64_t>(resident_count(tiers, kv.first)); }
}

void append_map_json(std::ostringstream & os, const std::map<std::string, uint64_t> & values) {
    os << "{";
    bool first = true;
    for (const auto & kv : values) {
        if (!first) os << ",";
        first = false;
        os << "\"" << ActivityRecord::escape_json(kv.first) << "\":" << kv.second;
    }
    os << "}";
}

void append_signed_map_json(std::ostringstream & os, const std::map<std::string, int64_t> & values) {
    os << "{";
    bool first = true;
    for (const auto & kv : values) {
        if (!first) os << ",";
        first = false;
        os << "\"" << ActivityRecord::escape_json(kv.first) << "\":" << kv.second;
    }
    os << "}";
}

void append_vector_json(std::ostringstream & os, const std::vector<std::string> & values) {
    os << "[";
    bool first = true;
    for (const auto & value : values) {
        if (!first) os << ",";
        first = false;
        os << "\"" << ActivityRecord::escape_json(value) << "\"";
    }
    os << "]";
}

} // namespace

std::string CacheIOSummary::to_json() const {
    std::ostringstream os;
    os << "{";
    os << "\"events\":" << events << ",";
    os << "\"transfer_events\":" << transfer_events << ",";
    os << "\"eviction_events\":" << eviction_events << ",";
    os << "\"writeback_events\":" << writeback_events << ",";
    os << "\"events_with_tokens\":" << events_with_tokens << ",";
    os << "\"events_with_pages\":" << events_with_pages << ",";
    os << "\"events_with_page_size\":" << events_with_page_size << ",";
    os << "\"events_with_bytes\":" << events_with_bytes << ",";
    os << "\"missing_bytes_events\":" << missing_bytes_events << ",";
    os << "\"estimated_latency_us\":" << estimated_latency_us << ",";
    os << "\"foreground_cache_io_us\":" << foreground_cache_io_us << ",";
    os << "\"background_cache_io_us\":" << background_cache_io_us << ",";
    os << "\"movement_events_used\":" << movement_events_used << ",";
    os << "\"observed_movements_used\":" << observed_movements_used << ",";
    os << "\"inferred_movements_used\":" << inferred_movements_used << ",";
    os << "\"model_generated_movements\":" << model_generated_movements << ",";
    os << "\"control_events_ignored\":" << control_events_ignored << ",";
    os << "\"hit_tokens_by_tier\":";
    append_map_json(os, hit_tokens_by_tier);
    os << ",\"hit_pages_by_tier\":";
    append_map_json(os, hit_pages_by_tier);
    os << ",\"miss_pages_by_tier\":";
    append_map_json(os, miss_pages_by_tier);
    os << ",\"bytes_by_edge\":";
    append_map_json(os, bytes_by_edge);
    os << ",\"pages_by_edge\":";
    append_map_json(os, pages_by_edge);
    os << ",\"resident_pages_by_tier\":";
    append_signed_map_json(os, resident_pages_by_tier);
    os << ",\"evictions_by_tier\":";
    append_map_json(os, evictions_by_tier);
    os << ",\"writebacks_by_edge\":";
    append_map_json(os, writebacks_by_edge);
    os << ",\"whatif_warnings\":";
    append_vector_json(os, whatif_warnings);
    os << "}";
    return os.str();
}

void CacheIOSummary::write_json(const std::string & filename) const {
    std::ofstream ofs(filename);
    if (!ofs.is_open()) { throw std::runtime_error("Failed to write model summary: " + filename); }
    ofs << to_json() << "\n";
}

CacheIOSummary apply_cache_io_model(TraceDAG & dag, const CacheIOConfig & config) {
    CacheIOSummary summary;
    if (!config.enabled) return summary;

    RuntimeTiers tiers;
    for (const auto & tier : config.tiers) {
        summary.resident_pages_by_tier.emplace(tier.name, 0);
        tiers.emplace(tier.name, TierRuntimeState {});
    }
    bool saw_observed_l3_to_l2_prefetch = false;
    bool saw_observed_l2_to_l1_load = false;
    bool saw_inferred_l3_to_l2 = false;
    bool saw_inferred_l2_to_l1 = false;

    for (auto & node : dag.nodes) {
        if (node.id >= dag.nodeid2recid.size()) continue;
        auto recid = dag.nodeid2recid[node.id];
        if (recid >= dag.records.size()) continue;
        auto & record = *dag.records[recid];
        if (!is_hicache_event(record)) continue;

        summary.events++;
        std::string kind = infer_event_kind(record);
        std::string src = infer_src(record);
        std::string dst = infer_dst(record);
        uint64_t pages = infer_pages(record, config);
        uint64_t bytes_per_page = infer_bytes_per_page(record, config);
        uint64_t bytes = infer_bytes(record, config, pages);
        uint64_t estimated = estimate_transfer_us(config, src, dst, bytes, record.dur);
        auto raw_keys = split_keys(arg_string(record, "page_keys_hash"));
        if (pages == 0 && !raw_keys.empty()) {
            pages = static_cast<uint64_t>(raw_keys.size());
            bytes = infer_bytes(record, config, pages);
            estimated = estimate_transfer_us(config, src, dst, bytes, record.dur);
        }
        auto direction = lower(arg_string(record, "direction"));
        bool physical_transfer_event = is_physical_transfer_event(record) && is_transfer(src, dst);
        bool state_movement_event = is_state_movement_event(record);
        bool observed_movement = status_completed(record) && (physical_transfer_event || state_movement_event);
        bool inferred_movement = false;
        bool replay_movement = observed_movement;
        bool transfer = replay_movement && is_transfer(src, dst);
        bool prefetch_edge = transfer && src == "L3" && dst == "L2" && direction == "prefetch";
        bool prefetch_ignored = prefetch_edge && !prefetch_event_is_enabled(config, record, pages);
        bool writeback_suppressed =
            transfer && write_back_policy(config) && src == "L2" && dst == "L3" && (direction == "write" || direction == "backup");
        bool async_prefetch = prefetch_edge && !should_count_transfer_foreground(config, record, src, dst, pages, estimated);
        if (observed_movement && src == "L3" && dst == "L2" && direction == "prefetch") saw_observed_l3_to_l2_prefetch = true;
        if (observed_movement && src == "L2" && dst == "L1" && direction == "load") saw_observed_l2_to_l1_load = true;
        if (inferred_movement && src == "L3" && dst == "L2") saw_inferred_l3_to_l2 = true;
        if (inferred_movement && src == "L2" && dst == "L1") saw_inferred_l2_to_l1 = true;

        if (!replay_movement) {
            pages = 0;
            bytes = 0;
            estimated = 0;
        }
        else if (!transfer) {
            bytes = 0;
            estimated = 0;
        }

        if (has_positive_arg(record, "num_tokens")) summary.events_with_tokens++;
        if (pages > 0) summary.events_with_pages++;
        if (has_positive_arg(record, "page_size") || config_page_size(config) > 0) summary.events_with_page_size++;
        if (transfer) {
            if (bytes > 0) summary.events_with_bytes++;
            else summary.missing_bytes_events++;
        }

        node.args["domain"] = "cache_io";
        node.args["cache_io.event_kind"] = replay_movement ? "movement" : kind;
        if (inferred_movement) node.args["cache_io.inferred"] = "1";
        node.args["cache_io.src"] = src;
        node.args["cache_io.dst"] = dst;
        node.args["cache_io.estimated_time"] = std::to_string(estimated);
        node.args["cache_io.pages"] = std::to_string(pages);
        node.args["cache_io.bytes"] = std::to_string(bytes);
        node.args["time"] = std::to_string((prefetch_ignored || async_prefetch || writeback_suppressed) ? 0 : estimated);
        if (node.args.find("ori_time") == node.args.end()) { node.args["ori_time"] = std::to_string(record.dur); }

        if (!replay_movement) {
            summary.control_events_ignored++;
        }
        else if (prefetch_ignored) {
            append_warning(summary, "prefetch policy ignored traced or inferred L3->L2 prefetch movement");
        }
        else if (writeback_suppressed) {
            append_warning(summary, "write_policy=write_back suppressed traced L2->L3 write-through movement");
        }
        else {
            summary.movement_events_used++;
            if (observed_movement) summary.observed_movements_used++;
            if (inferred_movement) summary.inferred_movements_used++;
        }

        auto keys = adjust_keys_for_pages(raw_keys, pages);
        if (!raw_keys.empty() && keys.size() != raw_keys.size()) {
            append_warning(summary, "page_size_policy=scenario adjusted page key cardinality");
        }

        if (transfer && !prefetch_ignored && !writeback_suppressed) {
            summary.transfer_events++;
            uint64_t latency_override = foreground_estimate_for_transfer(config, record, src, dst, bytes, pages, record.dur);
            add_edge_cost(summary, config, src, dst, pages, bytes, record.dur, !async_prefetch, async_prefetch, latency_override);

            uint64_t hit_pages = 0;
            uint64_t miss_pages = 0;
            if (!keys.empty()) {
                for (const auto & key : keys) {
                    bool hit = src == "L3" || has_page(tiers, src, key);
                    if (hit) {
                        hit_pages++;
                        touch_page(tiers, config, src, key);
                    }
                    else {
                        miss_pages++;
                    }
                }
            }
            else if (!src.empty() && pages > 0) {
                uint64_t resident = resident_count(tiers, src);
                hit_pages = std::min(resident, pages);
                miss_pages = pages > hit_pages ? pages - hit_pages : 0;
            }

            if (!src.empty()) {
                summary.hit_pages_by_tier[src] += hit_pages;
                summary.miss_pages_by_tier[src] += miss_pages;
            }

            if (src == "L2" && dst == "L1" && miss_pages > 0) {
                uint64_t demand_pages = miss_pages;
                if (keys.empty() && pages > 0) {
                    append_warning(summary, "cache_io cannot infer exact demand L3->L2 misses without page_keys_hash");
                }
                if (demand_pages > 0) {
                    uint64_t demand_bytes = edge_bytes_for_pages(bytes, pages, demand_pages, bytes_per_page);
                    add_edge_cost(summary, config, "L3", "L2", demand_pages, demand_bytes, record.dur);
                    summary.model_generated_movements++;
                    for (const auto & key : keys) {
                        if (!has_page(tiers, "L2", key)) touch_page(tiers, config, "L2", key);
                    }
                    enforce_capacity(summary, tiers, config, "L2", bytes_per_page);
                }
            }

            bool dst_dirty = dst == "L2" && src == "L1" && write_back_policy(config);
            if (!keys.empty()) {
                for (const auto & key : keys) {
                    touch_page(tiers, config, dst, key, dst_dirty);
                    if (src == "L2" && dst == "L3") mark_clean(tiers, "L2", key);
                }
            }
            else {
                add_anonymous_pages(tiers, dst, pages, dst_dirty);
            }
            enforce_capacity(summary, tiers, config, dst, bytes_per_page);
        }

        if (replay_movement && is_eviction(record)) {
            summary.eviction_events++;
            if (!src.empty()) {
                if (!keys.empty()) {
                    for (const auto & key : keys) { remove_page(tiers, src, key); }
                }
                else {
                    remove_anonymous_pages(tiers, src, pages);
                }
                summary.evictions_by_tier[src] += pages > 0 ? pages : 1;
            }
        }

        if (replay_movement && is_writeback(record) && !writeback_suppressed) {
            summary.writeback_events++;
            if (is_transfer(src, dst)) summary.writebacks_by_edge[src + "->" + dst]++;
        }

        auto status = arg_string(record, "status");
        auto hit_tier = arg_string(record, "hit_tier");
        if (hit_tier.empty()) {
            uint64_t device_hits = arg_u64(record, "device_hit_tokens", 0);
            uint64_t host_hits = arg_u64(record, "host_hit_tokens", 0);
            if (device_hits > 0) summary.hit_tokens_by_tier["L1"] += device_hits;
            if (host_hits > 0) summary.hit_tokens_by_tier["L2"] += host_hits;
        }
        else if (status.empty() || contains(status, "hit")) { summary.hit_tokens_by_tier[hit_tier] += arg_u64(record, "num_tokens", 0); }

        update_resident_summary(summary, tiers);
    }

    if (lower(config.prefetch_policy) == "none") {
        if (!saw_observed_l3_to_l2_prefetch && !saw_inferred_l3_to_l2) append_warning(summary, "prefetch_policy=none saw no traced or inferred L3->L2 prefetch movement");
        if (!saw_observed_l2_to_l1_load && !saw_inferred_l2_to_l1) append_warning(summary, "prefetch_policy=none saw no traced or inferred L2->L1 demand-load movement");
    }

    Logger::instance().info() << "CacheIO domain modeled: events=" << summary.events << " transfers=" << summary.transfer_events
                              << " estimated_latency_us=" << summary.estimated_latency_us;
    return summary;
}

} // namespace TraceGraph
