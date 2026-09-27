#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include <deque>
#include <stdexcept>

namespace hc = markov::trace_graph::modules::hicache;
using Kind = markov::trace_graph::core::DagEdgeKind;
namespace {
void require(bool value) { if (!value) throw std::runtime_error("Write page projection invariant failed"); }

hc::runtime::HiCacheWriteExpansion fixture(size_t pages) {
    hc::runtime::HiCacheWriteExpansion plan;
    const auto add = [&](const char * name, bool cpu, uint64_t duration) {
        const auto id = plan.nodes.size();
        hc::runtime::HiCacheHostExpansion::Node node;
        node.work.name = name;
        node.work.is_cpu = cpu;
        node.work.duration = duration;
        node.work.lane_key = cpu ? "main" : "write";
        plan.nodes.push_back(node);
        return id;
    };
    size_t tail = add("fixed preparation", true, 7);
    std::vector<size_t> submissions;
    for (size_t page = 0; page < pages; ++page) {
        const auto index = add("page index", true, 3);
        plan.edges.push_back({tail, index, Kind::Sequential});
        tail = index;
        for (int component = 0; component < 2; ++component) {
            const auto call = add("target write: AscendCL@aclrtMemcpy2dAsync", true, 5);
            plan.edges.push_back({tail, call, Kind::Sequential});
            submissions.push_back(call);
            tail = call;
        }
    }
    plan.host_return = add("fixed return", true, 11);
    plan.edges.push_back({tail, plan.host_return, Kind::Sequential});
    tail = plan.write_start = add("write start", false, 0);
    for (const auto call : submissions) {
        const auto payload = add("MEMCPY_ASYNC", false, 9);
        plan.payload.emplace(payload, 8);
        plan.edges.push_back({call, payload, Kind::Mutation});
        plan.edges.push_back({tail, payload, Kind::Stream});
        tail = payload;
    }
    plan.completion = add("write completion", false, 0);
    plan.edges.push_back({tail, plan.completion, Kind::Stream});
    plan.streams.push_back({0, plan.write_start, plan.completion});
    plan.payload_bytes = pages * 16;
    return plan;
}

void check(const hc::runtime::HiCacheWriteExpansion & plan, size_t pages, uint64_t page_bytes = 16) {
    require(plan.payload.size() == pages * 2 && plan.payload_bytes == pages * page_bytes);
    size_t calls = 0, preparation = 0, returns = 0;
    uint64_t cpu_cost = 0;
    for (const auto & node : plan.nodes) {
        calls += node.work.name.ends_with("aclrtMemcpy2dAsync");
        preparation += node.work.name == "fixed preparation";
        returns += node.work.name == "fixed return";
        if (node.work.is_cpu) cpu_cost += node.work.duration;
    }
    require(calls == 2 * pages && preparation == 1 && returns == 1 && cpu_cost == 18 + 13 * pages);
    std::vector<std::vector<size_t>> outgoing(plan.nodes.size());
    std::vector<size_t> degree(plan.nodes.size());
    for (const auto & edge : plan.edges) { outgoing.at(edge.from).push_back(edge.to); ++degree.at(edge.to); }
    std::deque<size_t> ready;
    for (size_t i = 0; i < degree.size(); ++i) if (!degree[i]) ready.push_back(i);
    size_t visited = 0;
    while (!ready.empty()) {
        const auto node = ready.front(); ready.pop_front(); ++visited;
        for (const auto next : outgoing[node]) if (!--degree[next]) ready.push_back(next);
    }
    require(visited == plan.nodes.size());
    const auto reaches = [&](size_t start, size_t goal, Kind kind) {
        std::vector<size_t> pending{start};
        std::vector<bool> seen(plan.nodes.size());
        while (!pending.empty()) {
            const auto node = pending.back(); pending.pop_back();
            if (node == goal) return true;
            if (seen[node]) continue;
            seen[node] = true;
            for (const auto & edge : plan.edges) if (edge.from == node && edge.kind == kind) pending.push_back(edge.to);
        }
        return false;
    };
    size_t previous_payload = plan.write_start;
    std::optional<size_t> previous_call;
    for (const auto & [payload, bytes] : plan.payload) {
        require(bytes == page_bytes / 2 && reaches(previous_payload, payload, Kind::Stream));
        std::optional<size_t> call;
        for (const auto & edge : plan.edges) if (edge.to == payload && edge.kind == Kind::Mutation) { require(!call); call = edge.from; }
        require(call.has_value());
        if (previous_call) require(reaches(*previous_call, *call, Kind::Sequential));
        previous_call = call;
        previous_payload = payload;
    }
    require(reaches(previous_payload, plan.completion, Kind::Stream));
    require(reaches(*previous_call, plan.host_return, Kind::Sequential));
}
} // namespace

void check_write_page_projection() {
    const auto original = fixture(23);
    require(hc::runtime::write_page_geometry_matches(original, 31 * 16, 16));
    require(hc::runtime::write_page_geometry_matches(fixture(1), 16, 16));
    require(!hc::runtime::write_page_geometry_matches(fixture(1), 32, 16));
    require(hc::runtime::write_page_geometry_matches(original, original.payload_bytes, 8));
    for (const uint64_t width : {8, 32, 64})
        for (const size_t pages : {1, 23, 31})
            check(hc::runtime::resize_write_pages(original, pages * width, width), pages, width);
    check(hc::runtime::resize_write_pages(original, original.payload_bytes, 8), 46, 8);
    require(!hc::runtime::write_page_geometry_matches(original, 15, 16));
    for (const size_t pages : {1, 15, 23, 24, 31}) check(hc::runtime::resize_write_pages(original, pages * 16, 16), pages);
    check(hc::runtime::resize_write_pages(hc::runtime::resize_write_pages(original, 31 * 16, 16), 15 * 16, 16), 15);
    bool rejected = false;
    try { (void)hc::runtime::resize_write_pages(fixture(1), 32, 16); }
    catch (const std::invalid_argument &) { rejected = true; }
    require(rejected); // A single first page cannot distinguish setup from a repeated body.
}
