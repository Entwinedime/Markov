#include "trace_graph/domains/cache_io/cache_model.hpp"

#include "trace_graph/activity_record.hpp"
#include "trace_graph/logger.hpp"
#include "trace_graph/trace_dag.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace TraceGraph {

namespace {

bool starts_with(const std::string & text, const std::string & prefix) { return text.rfind(prefix, 0) == 0; }

bool contains(const std::string & text, const std::string & needle) { return text.find(needle) != std::string::npos; }

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

void enforce_capacity(CacheIOSummary & summary, const CacheIOConfig & config, const std::string & tier_name) {
    auto tier = find_tier(config, tier_name);
    if (!tier || tier->capacity_infer || tier->capacity_infinite) return;
    auto & resident = summary.resident_pages_by_tier[tier_name];
    if (resident > static_cast<int64_t>(tier->capacity_pages)) {
        summary.eviction_events += static_cast<uint64_t>(resident - static_cast<int64_t>(tier->capacity_pages));
        resident = static_cast<int64_t>(tier->capacity_pages);
    }
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
    os << "\"hit_tokens_by_tier\":";
    append_map_json(os, hit_tokens_by_tier);
    os << ",\"bytes_by_edge\":";
    append_map_json(os, bytes_by_edge);
    os << ",\"resident_pages_by_tier\":";
    append_signed_map_json(os, resident_pages_by_tier);
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

    for (const auto & tier : config.tiers) { summary.resident_pages_by_tier.emplace(tier.name, 0); }

    for (auto & node : dag.nodes) {
        if (node.id >= dag.nodeid2recid.size()) continue;
        auto recid = dag.nodeid2recid[node.id];
        if (recid >= dag.records.size()) continue;
        auto & record = *dag.records[recid];
        if (!is_hicache_event(record)) continue;

        summary.events++;
        std::string src = infer_src(record);
        std::string dst = infer_dst(record);
        uint64_t pages = infer_pages(record, config);
        uint64_t bytes = infer_bytes(record, config, pages);
        uint64_t estimated = estimate_transfer_us(config, src, dst, bytes, record.dur);

        if (has_positive_arg(record, "num_tokens")) summary.events_with_tokens++;
        if (pages > 0) summary.events_with_pages++;
        if (has_positive_arg(record, "page_size") || parse_config_u64(config.page_size_tokens, 0) > 0) summary.events_with_page_size++;
        if (bytes > 0) summary.events_with_bytes++;
        else summary.missing_bytes_events++;

        node.args["domain"] = "cache_io";
        node.args["cache_io.src"] = src;
        node.args["cache_io.dst"] = dst;
        node.args["cache_io.estimated_time"] = std::to_string(estimated);
        node.args["cache_io.pages"] = std::to_string(pages);
        node.args["cache_io.bytes"] = std::to_string(bytes);
        node.args["time"] = std::to_string(estimated);
        if (node.args.find("ori_time") == node.args.end()) { node.args["ori_time"] = std::to_string(record.dur); }

        if (is_transfer(src, dst)) {
            summary.transfer_events++;
            summary.bytes_by_edge[src + "->" + dst] += bytes;
            auto src_tier = find_tier(config, src);
            if (!src_tier || !src_tier->capacity_infinite) {
                auto & resident_src = summary.resident_pages_by_tier[src];
                resident_src = std::max<int64_t>(0, resident_src - static_cast<int64_t>(pages));
            }
            summary.resident_pages_by_tier[dst] += static_cast<int64_t>(pages);
            enforce_capacity(summary, config, dst);
        }

        if (is_eviction(record)) {
            summary.eviction_events++;
            if (!src.empty()) {
                auto & resident = summary.resident_pages_by_tier[src];
                resident = std::max<int64_t>(0, resident - static_cast<int64_t>(pages));
            }
        }

        if (is_writeback(record)) summary.writeback_events++;

        auto status = arg_string(record, "status");
        auto hit_tier = arg_string(record, "hit_tier");
        if (hit_tier.empty()) {
            uint64_t device_hits = arg_u64(record, "device_hit_tokens", 0);
            uint64_t host_hits = arg_u64(record, "host_hit_tokens", 0);
            if (device_hits > 0) summary.hit_tokens_by_tier["L1"] += device_hits;
            if (host_hits > 0) summary.hit_tokens_by_tier["L2"] += host_hits;
        }
        else if (status.empty() || contains(status, "hit")) { summary.hit_tokens_by_tier[hit_tier] += arg_u64(record, "num_tokens", 0); }

        summary.estimated_latency_us += estimated;
    }

    Logger::instance().info() << "CacheIO domain modeled: events=" << summary.events << " transfers=" << summary.transfer_events
                              << " estimated_latency_us=" << summary.estimated_latency_us;
    return summary;
}

} // namespace TraceGraph
