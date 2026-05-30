#pragma once

#include "trace_graph/activity_record.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace TraceGraph {

enum class DAGEdgeType { Sequential, Stream, Correlation, Sync, HCCL, Virtual, Custom };

struct DAGEdge {
    size_t src;
    size_t dst;
    DAGEdgeType type;
    DAGEdge(size_t s, size_t d, DAGEdgeType t) : src(s), dst(d), type(t) {}
};

struct DAGNode {
    size_t id;
    size_t gpuflag;
    std::unordered_map<std::string, std::string> args;
    DAGNode(size_t id_, size_t flag_, const std::unordered_map<std::string, std::string> & args_ = {}) : id(id_), gpuflag(flag_), args(args_) {}
};

class TraceDAG {
public:
    std::vector<DAGNode> nodes;
    std::vector<DAGEdge> edges;
    std::vector<std::unique_ptr<ActivityRecord>> records;

    int gpu_id = 0;
    std::unordered_set<std::string> cputid;

    // --- Static factory methods ---
    static TraceDAG from_records(std::vector<std::unique_ptr<ActivityRecord>> && records, int gpu_id_ = 0);
    static TraceDAG merge_graphs(std::vector<TraceDAG> & graphs);

    // --- Pipeline methods ---
    void graphinit();
    void addCorrelationEdges();
    void addSequentialEdges();
    void joinEvent();
    void joinSync();
    void blockinganalysis();
    void simulation();
    void opt_scale(const std::string & name, double scale);

    // --- Output ---
    void to_chrome_tracing_json(const std::string & filename, bool concise = false, bool full_output = false);

    // --- Utility ---
    void addEdge(size_t node1, size_t node2, DAGEdgeType type);
    size_t createVirtual(std::string type, size_t recid);


    // --- Indexing maps (public for pipeline interop) ---
    std::unordered_map<std::string, std::vector<size_t>> connectionid2nodeid;
    std::unordered_map<std::string, std::vector<size_t>> correlationid2nodeid;
    std::vector<size_t> nodeid2recid;
    std::unordered_map<std::string, std::vector<size_t>> threadid2nodeid;

    std::vector<size_t> hcclkernel2nodeid;

    // --- Ascend Event tracking ---
    std::vector<size_t> event_record_nodes;
    std::vector<size_t> event_wait_nodes;
    std::vector<size_t> stream_sync_nodes;
    std::vector<size_t> notify_record_nodes;
    std::vector<size_t> notify_wait_nodes;
    std::vector<size_t> model_execute_nodes;

    std::unordered_map<std::string, std::vector<size_t>> eventId2nodeid;
    std::unordered_map<std::string, std::string> rawstream2stream;

    // --- Simulation state ---
    std::unordered_map<size_t, size_t> critical_pred;

    uint64_t e2e_time() const { return e2e_time_; }

private:
    uint64_t e2e_time_ = 0;
};

} // namespace TraceGraph
