#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {
namespace {
using Kind = core::DagEdgeKind;
using Edge = HiCacheHostExpansion::Edge;
bool ordered(Kind kind) { return kind == Kind::Sequential || kind == Kind::Stream; }

uint64_t observed_page_bytes(const HiCacheWriteExpansion & plan) {
    if (plan.payload.empty() || plan.payload.size() % 2) return 0;
    const auto pages = plan.payload.size() / 2;
    if (!plan.payload_bytes || plan.payload_bytes % pages) return 0;
    const auto bytes = plan.payload_bytes / pages;
    if (bytes % 2 || std::ranges::any_of(plan.payload, [&](const auto & entry) { return entry.second != bytes / 2; })) return 0;
    return bytes;
}

std::vector<size_t> page_submissions(const HiCacheWriteExpansion & plan, uint64_t page_bytes) {
    // Index only payload submissions once; page growth must not rescan every
    // template edge for each K/V transfer.
    std::map<size_t, size_t> submissions;
    for (const auto & edge : plan.edges) {
        if (edge.kind != Kind::Mutation || !plan.payload.contains(edge.to)) continue;

        if (!submissions.emplace(edge.to, edge.from).second) throw std::invalid_argument("Write payload has multiple submissions");
    }

    std::vector<size_t> calls;
    for (const auto & [payload, bytes] : plan.payload) {
        if (page_bytes % 2 || bytes != page_bytes / 2)
            throw std::invalid_argument("Write page projection requires equal K/V payloads at unchanged page geometry");

        const auto call = submissions.find(payload);
        if (call == submissions.end() || !plan.nodes.at(call->second).work.name.ends_with("AscendCL@aclrtMemcpy2dAsync"))
            throw std::invalid_argument("Write page projection requires an observed FAST2D submission");
        if (!calls.empty() && call->second <= calls.back()) throw std::invalid_argument("Write submissions are not in page order");

        calls.push_back(call->second);
    }
    return calls;
}

void check_cpu_block(const HiCacheWriteExpansion & plan, size_t begin, size_t end) {
    const auto & lane = plan.nodes.at(end).work.lane_key;
    for (size_t i = begin; i <= end; ++i)
        if (!plan.nodes.at(i).work.is_cpu || plan.nodes[i].work.lane_key != lane || plan.nodes[i].submission)
            throw std::invalid_argument("Write page block crosses a resource or queued task");
}

HiCacheWriteExpansion shrink(const HiCacheWriteExpansion & plan, size_t pages, const std::vector<size_t> & calls) {
    check_cpu_block(plan, calls[2 * pages - 1] + 1, calls.back());
    std::set<size_t> removed;
    for (size_t i = calls[2 * pages - 1] + 1; i <= calls.back(); ++i) removed.insert(i);
    size_t count = 0;
    for (const auto & [node, bytes] : plan.payload) if (count++ >= 2 * pages) removed.insert(node);
    HiCacheWriteExpansion result = plan;
    result.nodes.clear();
    result.edges.clear();
    result.payload.clear();
    result.source_nodes.clear();
    std::map<size_t, size_t> remap;
    for (size_t i = 0; i < plan.nodes.size(); ++i)
        if (!removed.contains(i)) {
            remap.emplace(i, result.nodes.size());
            result.nodes.push_back(plan.nodes[i]);
        }
    for (auto & node : result.nodes) if (node.submission) node.submission = remap.at(*node.submission);
    std::vector<std::vector<Edge>> outgoing(plan.nodes.size());
    for (const auto & edge : plan.edges) outgoing.at(edge.from).push_back(edge);
    for (const auto & [start, mapped] : remap) {
        auto pending = outgoing[start];
        std::set<std::pair<size_t, Kind>> visited;
        while (!pending.empty()) {
            const auto edge = pending.back();
            pending.pop_back();
            if (!visited.emplace(edge.to, edge.kind).second) continue;
            if (remap.contains(edge.to)) {
                result.edges.push_back({mapped, remap.at(edge.to), edge.kind});
                continue;
            }
            if (!ordered(edge.kind)) throw std::invalid_argument("Retained write work depends on a removed cross-resource task");
            for (const auto & next : outgoing[edge.to]) {
                if (next.kind == edge.kind) pending.push_back(next);
                else if (!removed.contains(next.to)) throw std::invalid_argument("Removed write page has an external output");
            }
        }
    }
    for (const auto & [node, bytes] : plan.payload) if (remap.contains(node)) result.payload.emplace(remap.at(node), bytes);
    for (const auto & [source, local] : plan.source_nodes) if (remap.contains(local)) result.source_nodes.emplace(source, remap.at(local));
    for (auto & stream : result.streams) { stream.first = remap.at(stream.first); stream.last = remap.at(stream.last); }
    for (auto & wait : result.waits) wait.consumer = remap.at(wait.consumer);
    for (auto & wait : result.event_waits) wait.consumer = remap.at(wait.consumer);
    result.host_return = remap.at(plan.host_return);
    result.write_start = remap.at(plan.write_start);
    result.completion = remap.at(plan.completion);
    return result;
}

HiCacheWriteExpansion grow(const HiCacheWriteExpansion & plan, size_t extra, const std::vector<size_t> & calls) {
    if (calls.size() < 4) throw std::invalid_argument("Write growth requires a measured non-first page");
    const auto before_cpu = calls[calls.size() - 3], last_cpu = calls.back();
    check_cpu_block(plan, before_cpu + 1, last_cpu);
    auto payload = plan.payload.end();
    const auto last = *--payload, first = *--payload;
    const auto before_payload = (--payload)->first;
    std::set<size_t> block{first.first, last.first};
    for (size_t i = before_cpu + 1; i <= last_cpu; ++i) block.insert(i);
    std::vector<Edge> incoming, outgoing, internal;
    HiCacheWriteExpansion result = plan;
    result.edges.clear();
    for (const auto & edge : plan.edges) {
        const bool from = block.contains(edge.from), to = block.contains(edge.to);
        if (from && !to) {
            if (!ordered(edge.kind) || (edge.from != last_cpu && edge.from != last.first))
                throw std::invalid_argument("Write steady page has an unhandled output");
            outgoing.push_back(edge);
        } else result.edges.push_back(edge);
        if (!from && to) {
            if (!ordered(edge.kind) || (edge.from != before_cpu && edge.from != before_payload))
                throw std::invalid_argument("Write steady page has an unhandled input");
            incoming.push_back(edge);
        }
        if (from && to) internal.push_back(edge);
    }
    size_t cpu_tail = last_cpu, payload_tail = last.first;
    std::map<size_t, size_t> remap;
    std::vector<size_t> added_cpu, added_payload;
    for (size_t page = 0; page < extra; ++page) {
        remap.clear();
        for (const auto old : block) {
            if (plan.nodes.at(old).submission) throw std::invalid_argument("Write repeated block contains a queued task");
            remap.emplace(old, result.nodes.size());
            (plan.nodes[old].work.is_cpu ? added_cpu : added_payload).push_back(result.nodes.size());
            result.nodes.push_back(plan.nodes[old]);
        }
        for (const auto & edge : internal) result.edges.push_back({remap.at(edge.from), remap.at(edge.to), edge.kind});
        for (const auto & edge : incoming)
            result.edges.push_back({edge.from == before_cpu ? cpu_tail : payload_tail, remap.at(edge.to), edge.kind});
        result.payload.emplace(remap.at(first.first), first.second);
        result.payload.emplace(remap.at(last.first), last.second);
        cpu_tail = remap.at(last_cpu);
        payload_tail = remap.at(last.first);
    }
    for (const auto & edge : outgoing) result.edges.push_back({remap.at(edge.from), edge.to, edge.kind});
    // Keep CPU call order in the template itself, including on later reuse.
    std::vector<size_t> order;
    for (size_t i = 0; i < plan.nodes.size(); ++i) {
        order.push_back(i);
        if (i == last_cpu) order.insert(order.end(), added_cpu.begin(), added_cpu.end());
        if (i == last.first) order.insert(order.end(), added_payload.begin(), added_payload.end());
    }
    remap.clear();
    auto nodes = std::move(result.nodes);
    result.nodes.clear();
    for (const auto old : order) { remap.emplace(old, result.nodes.size()); result.nodes.push_back(nodes.at(old)); }
    for (auto & node : result.nodes) if (node.submission) node.submission = remap.at(*node.submission);
    for (auto & edge : result.edges) { edge.from = remap.at(edge.from); edge.to = remap.at(edge.to); }
    const auto payloads = std::move(result.payload);
    result.payload.clear();
    for (const auto & [node, bytes] : payloads) result.payload.emplace(remap.at(node), bytes);
    for (auto & [source, node] : result.source_nodes) node = remap.at(node);
    for (auto & stream : result.streams) { stream.first = remap.at(stream.first); stream.last = remap.at(stream.last); }
    for (auto & wait : result.waits) wait.consumer = remap.at(wait.consumer);
    for (auto & wait : result.event_waits) wait.consumer = remap.at(wait.consumer);
    result.host_return = remap.at(result.host_return);
    result.write_start = remap.at(result.write_start);
    result.completion = remap.at(result.completion);
    return result;
}
} // namespace

bool write_page_geometry_matches(const HiCacheWriteExpansion & plan, uint64_t target_bytes, uint64_t page_bytes) {
    const bool fast2d = std::ranges::any_of(plan.nodes, [](const auto & node) {
        return node.work.name.ends_with("AscendCL@aclrtMemcpy2dAsync");
    });
    if (!fast2d) return target_bytes == plan.payload_bytes;
    if (!page_bytes || !target_bytes || page_bytes % 2 || target_bytes % page_bytes || !observed_page_bytes(plan))
        return false;
    return target_bytes / page_bytes <= plan.payload.size() / 2 || plan.payload.size() >= 4;
}

HiCacheWriteExpansion resize_write_pages(const HiCacheWriteExpansion & plan, uint64_t target_bytes, uint64_t page_bytes) {
    const bool fast2d = std::ranges::any_of(plan.nodes, [](const auto & node) {
        return node.work.name.ends_with("AscendCL@aclrtMemcpy2dAsync");
    });
    if (target_bytes == plan.payload_bytes && !fast2d) return plan;
    if (!write_page_geometry_matches(plan, target_bytes, page_bytes))
        throw std::invalid_argument("Write projection needs whole target pages and measured FAST2D K/V page work");
    const auto source_page_bytes = observed_page_bytes(plan);
    if (!source_page_bytes) throw std::invalid_argument("Write projection lacks source K/V page geometry");
    const auto original = plan.payload_bytes / source_page_bytes, target = target_bytes / page_bytes;
    const auto calls = page_submissions(plan, source_page_bytes);
    if (calls.size() % 2 || calls.size() / 2 != original) throw std::invalid_argument("Write template does not contain complete K/V pages");
    auto result = target == original ? plan : target < original ? shrink(plan, target, calls) : grow(plan, target - original, calls);
    // FAST2D submits K and V once per target page. Only cost estimates for the
    // CPU body/preparation are reused; transfer bytes and loop count are target
    // derived, and total device service is supplied later by the I/O model.
    for (auto & [node, bytes] : result.payload) bytes = page_bytes / 2;
    result.payload_bytes = target_bytes;
    return result;
}
} // namespace markov::trace_graph::modules::hicache::runtime
