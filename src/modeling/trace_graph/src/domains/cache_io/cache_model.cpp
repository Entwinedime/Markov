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

uint64_t infer_bytes_per_page(const ActivityRecord & record, const CacheIOConfig & config) {
    uint64_t bytes_per_page = arg_u64(record, "bytes_per_page", 0);
    if (bytes_per_page == 0) bytes_per_page = parse_config_u64(config.bytes_per_page, 0);
    if (bytes_per_page > 0) return bytes_per_page;

    uint64_t page_size = arg_u64(record, "page_size", 0);
    if (page_size == 0) page_size = parse_config_u64(config.page_size_tokens, 0);
    if (page_size == 0 || config.num_layers == 0 || config.num_kv_heads == 0 || config.head_dim == 0 || config.dtype_bytes == 0) return 0;

    uint64_t tp_size = std::max<uint64_t>(1, config.tp_size);
    uint64_t kv_heads_per_rank = (config.num_kv_heads + tp_size - 1) / tp_size;
    return page_size * config.num_layers * kv_heads_per_rank * config.head_dim * 2 * config.dtype_bytes;
}

uint64_t infer_bytes(const ActivityRecord & record, const CacheIOConfig & config, uint64_t pages) {
    uint64_t bytes = arg_u64(record, "bytes", 0);
    if (bytes > 0) return bytes;

    uint64_t bytes_per_page = infer_bytes_per_page(record, config);
    if (pages > 0 && bytes_per_page > 0) return pages * bytes_per_page;
    return 0;
}

uint64_t infer_pages(const ActivityRecord & record, const CacheIOConfig & config) {
    uint64_t pages = arg_u64(record, "num_pages", 0);
    if (pages > 0) return pages;

    uint64_t tokens = arg_u64(record, "num_tokens", 0);
    uint64_t page_size = arg_u64(record, "page_size", 0);
    if (page_size == 0) page_size = parse_config_u64(config.page_size_tokens, 0);
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

void append_warning(CacheIOSummary & summary, const std::string & warning) {
    if (std::find(summary.whatif_warnings.begin(), summary.whatif_warnings.end(), warning) == summary.whatif_warnings.end()) {
        summary.whatif_warnings.push_back(warning);
    }
}

struct TierRuntimeState {
    std::unordered_set<std::string> resident;
    std::deque<std::string> order;
    uint64_t anonymous_pages = 0;
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

void touch_page(RuntimeTiers & tiers, const CacheIOConfig & config, const std::string & tier_name, const std::string & key) {
    if (tier_name.empty() || key.empty()) return;
    auto & state = tiers[tier_name];
    auto tier = find_tier(config, tier_name);
    bool existed = state.resident.find(key) != state.resident.end();
    state.resident.insert(key);

    if (tier && lower(tier->eviction) == "fifo" && existed) return;
    remove_from_order(state, key);
    state.order.push_back(key);
}

void remove_page(RuntimeTiers & tiers, const std::string & tier_name, const std::string & key) {
    auto it = tiers.find(tier_name);
    if (it == tiers.end()) return;
    it->second.resident.erase(key);
    remove_from_order(it->second, key);
}

void add_anonymous_pages(RuntimeTiers & tiers, const std::string & tier_name, uint64_t pages) {
    if (tier_name.empty() || pages == 0) return;
    tiers[tier_name].anonymous_pages += pages;
}

void remove_anonymous_pages(RuntimeTiers & tiers, const std::string & tier_name, uint64_t pages) {
    auto it = tiers.find(tier_name);
    if (it == tiers.end() || pages == 0) return;
    it->second.anonymous_pages = it->second.anonymous_pages > pages ? it->second.anonymous_pages - pages : 0;
}

uint64_t resident_count(const RuntimeTiers & tiers, const std::string & tier_name) {
    auto it = tiers.find(tier_name);
    if (it == tiers.end()) return 0;
    return static_cast<uint64_t>(it->second.resident.size()) + it->second.anonymous_pages;
}

void enforce_capacity(CacheIOSummary & summary, RuntimeTiers & tiers, const CacheIOConfig & config, const std::string & tier_name) {
    auto tier = find_tier(config, tier_name);
    if (!tier || tier->capacity_infer || tier->capacity_infinite) return;
    auto & state = tiers[tier_name];
    while (resident_count(tiers, tier_name) > tier->capacity_pages) {
        if (state.resident.empty() && state.anonymous_pages == 0) break;

        if (lower(tier->eviction) == "infinite") break;
        if (!state.order.empty()) {
            std::string victim = state.order.front();
            state.order.pop_front();
            state.resident.erase(victim);
        }
        else if (state.anonymous_pages > 0) {
            state.anonymous_pages--;
        }
        else {
            auto it = state.resident.begin();
            state.resident.erase(it);
        }
        summary.eviction_events++;
        summary.evictions_by_tier[tier_name]++;
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
                   uint64_t observed = 0,
                   bool count_latency = true) {
    if (!is_transfer(src, dst)) return;
    std::string edge = src + "->" + dst;
    summary.pages_by_edge[edge] += pages;
    summary.bytes_by_edge[edge] += bytes;
    if (count_latency) summary.estimated_latency_us += estimate_transfer_us(config, src, dst, bytes, observed);
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
    os << "\"movement_events_used\":" << movement_events_used << ",";
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
    bool saw_l3_to_l2_prefetch = false;
    bool saw_l2_to_l1_load = false;

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
        bool movement = kind == "movement";
        bool transfer = movement && is_transfer(src, dst);
        bool prefetch_ignored = transfer && lower(config.prefetch_policy) == "none" && src == "L3" && dst == "L2";
        bool async_prefetch = transfer && lower(arg_string(record, "direction")) == "prefetch";
        if (movement && src == "L3" && dst == "L2" && lower(arg_string(record, "direction")) == "prefetch") saw_l3_to_l2_prefetch = true;
        if (movement && src == "L2" && dst == "L1" && lower(arg_string(record, "direction")) == "load") saw_l2_to_l1_load = true;

        if (has_positive_arg(record, "num_tokens")) summary.events_with_tokens++;
        if (pages > 0) summary.events_with_pages++;
        if (has_positive_arg(record, "page_size") || parse_config_u64(config.page_size_tokens, 0) > 0) summary.events_with_page_size++;
        if (bytes > 0) summary.events_with_bytes++;
        else summary.missing_bytes_events++;

        node.args["domain"] = "cache_io";
        node.args["cache_io.event_kind"] = kind;
        node.args["cache_io.src"] = src;
        node.args["cache_io.dst"] = dst;
        node.args["cache_io.estimated_time"] = std::to_string(estimated);
        node.args["cache_io.pages"] = std::to_string(pages);
        node.args["cache_io.bytes"] = std::to_string(bytes);
        node.args["time"] = std::to_string((prefetch_ignored || async_prefetch) ? 0 : estimated);
        if (node.args.find("ori_time") == node.args.end()) { node.args["ori_time"] = std::to_string(record.dur); }

        if (!movement) {
            summary.control_events_ignored++;
        }
        else if (prefetch_ignored) {
            append_warning(summary, "prefetch_policy=none ignored traced L3->L2 prefetch movement");
        }
        else {
            summary.movement_events_used++;
        }

        auto keys = split_keys(arg_string(record, "page_keys_hash"));

        if (transfer && !prefetch_ignored) {
            summary.transfer_events++;
            add_edge_cost(summary, config, src, dst, pages, bytes, record.dur, !async_prefetch);

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

            if (src == "L2" && dst == "L1" && lower(config.prefetch_policy) == "none") {
                uint64_t demand_pages = miss_pages;
                if (keys.empty() && pages > 0) {
                    append_warning(summary, "prefetch_policy=none cannot infer demand L3->L2 misses without page_keys_hash");
                }
                if (demand_pages > 0) {
                    uint64_t demand_bytes = edge_bytes_for_pages(bytes, pages, demand_pages, bytes_per_page);
                    add_edge_cost(summary, config, "L3", "L2", demand_pages, demand_bytes, record.dur);
                    for (const auto & key : keys) {
                        if (!has_page(tiers, "L2", key)) touch_page(tiers, config, "L2", key);
                    }
                    enforce_capacity(summary, tiers, config, "L2");
                }
            }

            if (!keys.empty()) {
                for (const auto & key : keys) { touch_page(tiers, config, dst, key); }
            }
            else {
                add_anonymous_pages(tiers, dst, pages);
            }
            enforce_capacity(summary, tiers, config, dst);
        }

        if (movement && is_eviction(record)) {
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

        if (movement && is_writeback(record)) {
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

        if (movement && !prefetch_ignored && !transfer) summary.estimated_latency_us += estimated;
        update_resident_summary(summary, tiers);
    }

    if (lower(config.prefetch_policy) == "none") {
        if (!saw_l3_to_l2_prefetch) append_warning(summary, "prefetch_policy=none saw no traced L3->L2 prefetch movement");
        if (!saw_l2_to_l1_load) append_warning(summary, "prefetch_policy=none saw no traced L2->L1 demand-load movement");
    }

    Logger::instance().info() << "CacheIO domain modeled: events=" << summary.events << " transfers=" << summary.transfer_events
                              << " estimated_latency_us=" << summary.estimated_latency_us;
    return summary;
}

} // namespace TraceGraph
