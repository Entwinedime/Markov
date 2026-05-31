#include "trace_graph/trace_dag.hpp"
#include "trace_graph/export_raw_trace.hpp"
#include "trace_graph/logger.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace TraceGraph {

namespace {

bool starts_with(const std::string & text, const std::string & prefix) { return text.rfind(prefix, 0) == 0; }

bool is_cache_io_record(const ActivityRecord & record) {
    auto domain = record.args.find("domain");
    return record.cat == "hicache" || starts_with(record.name, "HiCache::") || (domain != record.args.end() && domain->second == "cache_io");
}

} // namespace

// ======================================================================
//  from_records  -- main pipeline
// ======================================================================

TraceDAG TraceDAG::from_records(std::vector<std::unique_ptr<ActivityRecord>> && in_records, int gpu_id_) {
    TraceDAG g;
    g.gpu_id = gpu_id_;
    bool debug_mode = (std::getenv("DEBUG_TRACE") != nullptr);

    // --- Step 0: Deduplicate records with same ts and dur ---
    std::unordered_map<std::string, std::vector<size_t>> ts_dur_groups;
    for (size_t i = 0; i < in_records.size(); ++i) {
        std::string key = std::to_string(in_records[i]->ts) + "_" + std::to_string(in_records[i]->dur);
        ts_dur_groups[key].push_back(i);
    }

    std::vector<bool> to_delete(in_records.size(), false);
    for (auto & kv : ts_dur_groups) {
        auto & indices = kv.second;
        if (indices.size() > 1) {
            size_t npu_idx = static_cast<size_t>(-1);
            for (size_t idx : indices) {
                if (in_records[idx]->args.count("Physic Stream Id")) {
                    npu_idx = idx;
                    break;
                }
            }
            if (npu_idx != static_cast<size_t>(-1)) {
                for (size_t idx : indices) {
                    if (idx != npu_idx) {
                        for (const auto & arg_kv : in_records[idx]->args) { in_records[npu_idx]->args[arg_kv.first] = arg_kv.second; }
                        std::string tid = "-1";
                        if (in_records[idx]->args.count("tid")) tid = in_records[idx]->args.at("tid");
                        if (tid == "0") { in_records[npu_idx]->name = in_records[idx]->name; }
                        to_delete[idx] = true;
                    }
                }
            }
        }
    }

    std::vector<std::unique_ptr<ActivityRecord>> merged_records;
    for (size_t i = 0; i < in_records.size(); ++i) {
        if (!to_delete[i]) { merged_records.push_back(std::move(in_records[i])); }
    }
    in_records = std::move(merged_records);

    // --- Step 1: Sort by timestamp ---
    std::sort(in_records.begin(), in_records.end(), [](const std::unique_ptr<ActivityRecord> & a, const std::unique_ptr<ActivityRecord> & b) {
        if (a->ts != b->ts) return a->ts < b->ts;
        return a->dur > b->dur;
    });

    // --- Step 2: Group by thread/stream ---
    std::unordered_map<std::string, std::vector<size_t>> tid_to_idx;
    std::unordered_set<std::string> cpu_tids;

    for (size_t i = 0; i < in_records.size(); ++i) {
        auto & r = in_records[i];

        bool is_npu = false;
        if (r->args.count("Physic Stream Id")) { is_npu = true; }
        else if (r->cat == "Kernel" || r->cat == "cpu_op") {
            if (r->args.count("streamId")) is_npu = true;
        }

        std::string group_key;
        if (is_npu) {
            if (r->args.count("streamId")) group_key = r->args.at("streamId");
            else if (r->args.count("Physic Stream Id")) group_key = r->args.at("Physic Stream Id");
            else group_key = "NPU_UNKNOWN";
        }
        else { group_key = "CPU_MERGED"; }

        tid_to_idx[group_key].push_back(i);

        if (!is_npu && r->ts) cpu_tids.insert(group_key);
    }

    // --- Step 3: Leaf node extraction ---
    std::vector<bool> is_leaf(in_records.size(), true);
    std::vector<bool> is_discarded(in_records.size(), false);
    std::vector<bool> is_enqueue_node(in_records.size(), false);
    std::unordered_set<std::string> seen_corr_ids;

    for (auto & kv : tid_to_idx) {
        const std::string & tid = kv.first;
        auto & indices = kv.second;

        if (cpu_tids.count(tid)) {
            std::vector<size_t> stack_idx;
            for (size_t i = 0; i < indices.size(); ++i) {
                size_t curr_idx = indices[i];
                auto & curr_rec = in_records[curr_idx];

                if (curr_rec->args.count("correlation_id")) {
                    std::string cid = curr_rec->args.at("correlation_id");
                    if (seen_corr_ids.count(cid) == 0) {
                        is_enqueue_node[curr_idx] = true;
                        seen_corr_ids.insert(cid);
                    }
                }

                while (!stack_idx.empty()) {
                    size_t top_idx = stack_idx.back();
                    auto & top_rec = in_records[top_idx];

                    double curr_end_threshold = curr_rec->ts + curr_rec->dur * 0.5;
                    double top_end = top_rec->ts + top_rec->dur;

                    if (top_end > curr_end_threshold) {
                        if (top_rec->name == "Node@launch") {
                            is_discarded[curr_idx] = true;
                            break;
                        }

                        if (is_enqueue_node[top_idx]) {
                            is_discarded[curr_idx] = true;
                            break;
                        }

                        if (curr_rec->name.rfind("Runtime@", 0) == 0) {
                            is_discarded[curr_idx] = true;
                            break;
                        }
                        if (top_rec->name != "AscendCL@aclrtRecordEvent") is_leaf[top_idx] = false;
                        if (top_rec->args.count("correlation_id") && !curr_rec->args.count("correlation_id")) {
                            curr_rec->args["correlation_id"] = top_rec->args.at("correlation_id");
                        }
                        break;
                    }
                    else { stack_idx.pop_back(); }
                }

                if (is_discarded[curr_idx]) continue;

                if (!stack_idx.empty()) {
                    std::string parent_seq = "";
                    for (size_t j = 0; j < stack_idx.size(); ++j) {
                        size_t p_idx = stack_idx[j];
                        auto & p_rec = in_records[p_idx];
                        if (j > 0) parent_seq += " -> ";
                        parent_seq += p_rec->name + "(" + std::to_string(p_idx) + ")";
                    }
                    curr_rec->args["parent_seq"] = parent_seq;
                }

                stack_idx.push_back(curr_idx);
            }
        }
    }

    // --- Step 4: Filter to leaf nodes only ---
    std::vector<std::unique_ptr<ActivityRecord>> filtered_records;
    for (size_t i = 0; i < in_records.size(); ++i) {
        if ((is_leaf[i] && !is_discarded[i]) || is_cache_io_record(*in_records[i])) { filtered_records.push_back(std::move(in_records[i])); }
    }

    g.records = std::move(filtered_records);

    if (debug_mode) {
        std::string out_name = "debug_1_leaf_nodes_" + std::to_string(gpu_id_) + ".json";
        export_raw_trace(out_name, g.records);
        Logger::instance().debug() << "Exported leaf nodes to " << out_name;
    }

    // --- Step 5: Build graph ---
    g.graphinit();
    g.addCorrelationEdges();
    g.addSequentialEdges();
    if (debug_mode) {
        g.simulation();
        std::string out_name = "debug_3_sequential_edges_" + std::to_string(gpu_id_) + ".json";
        g.to_chrome_tracing_json(out_name, true);
        Logger::instance().debug() << "Exported sequential edges graph to " << out_name;
    }
    g.joinEvent();
    g.blockinganalysis();
    if (debug_mode) {
        g.simulation();
        std::string out_name = "debug_5_blocking_edges_" + std::to_string(gpu_id_) + ".json";
        g.to_chrome_tracing_json(out_name, true);
        Logger::instance().debug() << "Exported blocking edges graph to " << out_name;
    }
    g.joinSync();

    return g;
}

// ======================================================================
//  graphinit
// ======================================================================

void TraceDAG::graphinit() {
    nodes.clear();
    edges.clear();

    std::string cpu_merged_pid = "-1";
    std::string cpu_merged_tid = "-1";

    for (size_t i = 0; i < records.size(); i++) {
        const auto & r = records[i];
        size_t node_id = nodes.size();
        size_t flag = -1;

        if (r->args.count("Physic Stream Id")) {
            try {
                flag = std::stoi(r->args.at("Physic Stream Id"));
            }
            catch (...) {
                flag = 1;
            }
        }
        else if (r->cat == "Kernel" || r->cat == "cpu_op") {
            if (r->args.count("streamId")) {
                try {
                    flag = std::stoi(r->args.at("streamId"));
                }
                catch (...) {
                }
            }
            else if (r->args.count("tid")) { flag = -1; }
        }

        nodes.emplace_back(node_id, flag);

        if (r->args.count("parent_seq")) { nodes.back().args["parent_seq"] = r->args.at("parent_seq"); }

        std::string tid = "-1";
        if (r->args.count("tid")) tid = r->args.at("tid");
        else if (r->args.count("streamId")) tid = r->args.at("streamId");

        std::string pid = "-1";
        if (r->args.count("pid")) pid = r->args.at("pid");
        else if (r->args.count("processId")) pid = r->args.at("processId");
        else if (r->args.count("deviceId")) pid = r->args.at("deviceId");

        if (flag == -1) {
            if (cpu_merged_pid == "-1") {
                cpu_merged_pid = pid;
                cpu_merged_tid = tid;
            }
            threadid2nodeid["CPU_MERGED"].emplace_back(node_id);
            cputid.insert("CPU_MERGED");
            nodes.back().args["sim_pid"] = cpu_merged_pid;
            nodes.back().args["sim_tid"] = cpu_merged_tid;
        }
        else { threadid2nodeid[tid].emplace_back(node_id); }

        if (r->args.count("connection_id")) { connectionid2nodeid[r->args.at("connection_id")].emplace_back(node_id); }
        if (r->args.count("correlation_id")) { correlationid2nodeid[r->args.at("correlation_id")].emplace_back(node_id); }

        if (flag != -1
            && (r->name.find("hcom") != std::string::npos || r->name.find("HCCL") != std::string::npos || r->name.find("hccl") != std::string::npos)) {
            hcclkernel2nodeid.emplace_back(node_id);
        }

        if (r->name == "EVENT_RECORD") { event_record_nodes.push_back(node_id); }
        else if (r->name == "EVENT_WAIT") {
            r->ts += 1;
            r->dur -= 1;
            event_wait_nodes.push_back(node_id);
        }
        else if (r->name == "AscendCL@aclrtSynchronizeStream") { stream_sync_nodes.push_back(node_id); }
        else if (r->name == "NOTIFY_RECORD") { notify_record_nodes.push_back(node_id); }
        else if (r->name == "MODEL_EXECUTE") { model_execute_nodes.push_back(node_id); }

        nodeid2recid.emplace_back(i);
    }
}

// ======================================================================
//  addCorrelationEdges
// ======================================================================

void TraceDAG::addCorrelationEdges() {
    for (auto & p : correlationid2nodeid) {
        auto & nodeidlist = p.second;
        if (nodeidlist.size() <= 1) continue;
        std::sort(nodeidlist.begin(), nodeidlist.end(), [this](const size_t & a, const size_t & b) {
            if (records[nodeid2recid[a]]->ts != records[nodeid2recid[b]]->ts) return records[nodeid2recid[a]]->ts < records[nodeid2recid[b]]->ts;
            return a < b;
        });

        for (size_t i = 1; i < nodeidlist.size(); ++i) {
            records[nodeid2recid[nodeidlist[i]]]->args["launchts"] = std::to_string(records[nodeid2recid[nodeidlist[0]]]->ts);
            addEdge(nodeidlist[i - 1], nodeidlist[i], DAGEdgeType::Correlation);
        }
    }

    for (auto & p : connectionid2nodeid) {
        auto & nodeidlist = p.second;
        if (nodeidlist.size() <= 1 || (nodeidlist.size() >= 3 && records[nodeid2recid[nodeidlist[0]]]->name != "Node@launch")) continue;
        std::sort(nodeidlist.begin(), nodeidlist.end(), [this](const size_t & a, const size_t & b) {
            if (records[nodeid2recid[a]]->ts != records[nodeid2recid[b]]->ts) return records[nodeid2recid[a]]->ts < records[nodeid2recid[b]]->ts;
            return a < b;
        });

        uint64_t launchts = records[nodeid2recid[nodeidlist[0]]]->ts;
        for (size_t i = 0; i < nodeidlist.size(); ++i) { records[nodeid2recid[nodeidlist[i]]]->args["launchts"] = std::to_string(launchts); }

        for (size_t i = 1; i < nodeidlist.size(); ++i) { addEdge(nodeidlist[i - 1], nodeidlist[i], DAGEdgeType::Correlation); }
    }
}

// ======================================================================
//  addSequentialEdges
// ======================================================================

void TraceDAG::addSequentialEdges() {
    for (auto & p : threadid2nodeid) {
        auto & nodeidlist = p.second;
        bool is_cpu = cputid.count(p.first) > 0;

        std::sort(nodeidlist.begin(), nodeidlist.end(), [this](const size_t & a, const size_t & b) {
            if (records[nodeid2recid[a]]->ts != records[nodeid2recid[b]]->ts) return records[nodeid2recid[a]]->ts < records[nodeid2recid[b]]->ts;
            return a < b;
        });

        if (nodeidlist.empty()) continue;

        nodes[nodeidlist[0]].args["time"] = std::to_string(records[nodeid2recid[nodeidlist[0]]]->dur);
        nodes[nodeidlist[0]].args["ori_time"] = nodes[nodeidlist[0]].args["time"];
        nodes[nodeidlist[0]].args["tid"] = p.first;
        if (!is_cpu) nodes[nodeidlist[0]].args["gpuid"] = std::to_string(gpu_id);

        for (size_t i = 1; i < nodeidlist.size(); ++i) {
            auto & r = records[nodeid2recid[nodeidlist[i]]];
            nodes[nodeidlist[i]].args["cat"] = r->cat;
            nodes[nodeidlist[i]].args["name"] = r->name;
            nodes[nodeidlist[i]].args["time"] = std::to_string(r->dur);
            nodes[nodeidlist[i]].args["ori_time"] = nodes[nodeidlist[i]].args["time"];
            nodes[nodeidlist[i]].args["tid"] = p.first;
            if (!is_cpu) nodes[nodeidlist[i]].args["gpuid"] = std::to_string(gpu_id);

            if (is_cpu) {
                auto & ts1 = records[nodeid2recid[nodeidlist[i - 1]]]->ts;
                auto & dur1 = records[nodeid2recid[nodeidlist[i - 1]]]->dur;
                uint64_t cpu_interval = (r->ts > ts1 + dur1) ? (r->ts - (ts1 + dur1)) : 0;
                nodes[nodeidlist[i - 1]].args["cpuinterval"] = std::to_string(cpu_interval);
            }

            addEdge(nodeidlist[i - 1], nodeidlist[i], DAGEdgeType::Sequential);

            auto & prev = records[nodeid2recid[nodeidlist[i - 1]]];

            if (r->name == "NOTIFY_WAIT" && prev->name == "MODEL_EXECUTE") { notify_wait_nodes.push_back(nodeidlist[i]); }

            if ((prev->name.find("hcom") != std::string::npos || prev->name.find("HCCL") != std::string::npos || prev->name.find("hccl") != std::string::npos)
                && !is_cpu) {
                nodes[nodeidlist[i - 1]].args["hccl_sync"] = std::to_string(nodeidlist[i]);
            }
        }
    }
}

// ======================================================================
//  addEdge / createVirtual
// ======================================================================

void TraceDAG::addEdge(size_t node1, size_t node2, DAGEdgeType type) { edges.emplace_back(node1, node2, type); }

size_t TraceDAG::createVirtual(std::string type, size_t recid) {
    std::unordered_map<std::string, std::string> args;
    records.emplace_back(std::make_unique<VirtualRecord>(type, 0, 1, args));
    size_t node_id = nodes.size();
    nodes.emplace_back(node_id, gpu_id);
    nodeid2recid.emplace_back(records.size() - 1);
    records[recid]->args["virtualId"] = std::to_string(node_id);
    return node_id;
}

// ======================================================================
//  joinEvent
// ======================================================================

void TraceDAG::joinEvent() {
    for (size_t node_id : event_record_nodes) {
        auto connlist = connectionid2nodeid[records[nodeid2recid[node_id]]->args["connection_id"]];
        auto cpu_node_id = connlist[0];
        auto & rec = records[nodeid2recid[cpu_node_id]];
        auto & rec_npu = records[nodeid2recid[node_id]];

        rawstream2stream[rec->args["Raw Stream"]] = rec_npu->args["Physic Stream Id"];
        std::string eid = "0";
        if (rec->args.count("Event Id")) eid = rec->args["Event Id"];
        eventId2nodeid[eid].push_back(node_id);
    }
    for (auto & p : eventId2nodeid) {
        std::sort(p.second.begin(), p.second.end(), [&](size_t a, size_t b) { return records[nodeid2recid[a]]->ts < records[nodeid2recid[b]]->ts; });
    }

    std::sort(notify_record_nodes.begin(), notify_record_nodes.end(), [&](size_t a, size_t b) {
        return records[nodeid2recid[a]]->ts < records[nodeid2recid[b]]->ts;
    });
}

// ======================================================================
//  joinSync
// ======================================================================

void TraceDAG::joinSync() {
    for (size_t node_id : stream_sync_nodes) {
        records[nodeid2recid[node_id]]->dur = 10;
        nodes[node_id].args["time"] = "10";
        nodes[node_id].args["ori_time"] = "10";
    }
    for (size_t node_id : event_wait_nodes) {
        records[nodeid2recid[node_id]]->dur = 10;
        nodes[node_id].args["time"] = "10";
        nodes[node_id].args["ori_time"] = "10";
    }
    for (size_t node_id : notify_wait_nodes) {
        records[nodeid2recid[node_id]]->dur = 10;
        nodes[node_id].args["time"] = "10";
        nodes[node_id].args["ori_time"] = "10";
    }
}

// ======================================================================
//  blockinganalysis
// ======================================================================

void TraceDAG::blockinganalysis() {
    // --- a) StreamWaitEvent ---
    for (size_t node_id : event_wait_nodes) {
        auto cpu_node_id = connectionid2nodeid[records[nodeid2recid[node_id]]->args["connection_id"]][0];
        auto & rec = records[nodeid2recid[cpu_node_id]];
        auto & rec_npu = records[nodeid2recid[node_id]];

        std::string eid = "0";
        if (rec->args.count("Event Id")) eid = rec->args["Event Id"];

        if (eventId2nodeid.count(eid)) {
            auto & list = eventId2nodeid[eid];
            auto it = std::upper_bound(list.begin(), list.end(), rec_npu->ts + rec_npu->dur - 0.1, [&](uint64_t val, size_t n) {
                return val < records[nodeid2recid[n]]->ts;
            });
            if (it != list.begin()) {
                --it;
                while (it != list.begin() && nodes[node_id].args["tid"] == nodes[*it].args["tid"]) { --it; }
                addEdge(*it, node_id, DAGEdgeType::Sync);
            }
        }
    }

    // --- b) Notify Wait When Finish Model Execute ---
    for (size_t node_id : notify_wait_nodes) {
        auto & wait_rec = records[nodeid2recid[node_id]];
        uint64_t wait_end = wait_rec->ts + wait_rec->dur;

        auto it = std::upper_bound(notify_record_nodes.begin(), notify_record_nodes.end(), wait_end - 200, [&](uint64_t val, size_t n) {
            return val < records[nodeid2recid[n]]->ts;
        });

        if (it != notify_record_nodes.begin()) {
            --it;
            addEdge(*it, node_id, DAGEdgeType::Sync);
        }
    }

    // --- c) Model Execute Asynchronize ---
    for (size_t me_node : model_execute_nodes) {
        auto & me_rec = records[nodeid2recid[me_node]];
        uint64_t me_start = me_rec->ts;
        uint64_t nw_end = me_start;

        for (size_t nw_node : notify_wait_nodes) {
            auto & nw_rec = records[nodeid2recid[nw_node]];
            if (nw_rec->ts >= me_start) {
                if (nw_end == me_start || (nw_rec->ts + nw_rec->dur) < nw_end) { nw_end = nw_rec->ts + nw_rec->dur; }
            }
        }
        if (nw_end > me_start) {
            for (const auto & kv : threadid2nodeid) {
                const std::string & tid = kv.first;
                const auto & nlist = kv.second;
                if (cputid.count(tid) || tid == me_rec->args["tid"]) continue;
                for (size_t n : nlist) {
                    auto & n_rec = records[nodeid2recid[n]];
                    if (n_rec->ts >= me_start && n_rec->ts <= nw_end) {
                        addEdge(me_node, n, DAGEdgeType::Sync);
                        break;
                    }
                }
            }
        }
    }

    // --- d) StreamSynchronize ---
    for (size_t node_id : stream_sync_nodes) {
        auto & rec = records[nodeid2recid[node_id]];
        std::string sid = "-1";
        if (rec->args.count("streamId")) sid = rec->args["streamId"];
        else if (rec->args.count("stream id")) sid = rec->args["stream id"];
        else if (rec->args.count("Physic Stream Id")) sid = rec->args["Physic Stream Id"];
        else if (rec->args.count("Raw Stream")) sid = rawstream2stream[rec->args["Raw Stream"]];

        std::vector<std::string> target_streams;
        if (sid != "-1" && threadid2nodeid.count(sid)) { target_streams.push_back(sid); }
        else if (sid == "-1") {
            for (const auto & kv : threadid2nodeid) {
                if (cputid.count(kv.first) == 0) { target_streams.push_back(kv.first); }
            }
        }

        for (const std::string & target_sid : target_streams) {
            auto & list = threadid2nodeid[target_sid];

            auto it = std::lower_bound(list.begin(), list.end(), rec->ts, [&](size_t n, uint64_t val) {
                auto & n_rec = records[nodeid2recid[n]];
                uint64_t n_launchts = n_rec->ts;
                if (n_rec->args.count("launchts")) {
                    try {
                        n_launchts = std::stoull(n_rec->args.at("launchts"));
                    }
                    catch (...) {
                    }
                }
                return n_launchts < val;
            });

            if (it != list.begin()) {
                --it;
                addEdge(*it, node_id, DAGEdgeType::Sync);
            }
        }
    }
}

// ======================================================================
//  opt_scale
// ======================================================================

void TraceDAG::opt_scale(const std::string & name, double scale) {
    for (auto & node : nodes) {
        if (node.id >= nodeid2recid.size()) continue;
        auto & rec = records[nodeid2recid[node.id]];

        if (rec->name.find(name) != std::string::npos) {
            if (node.args.count("ori_time")) {
                try {
                    uint64_t ori_time = std::stoull(node.args["ori_time"]);
                    uint64_t new_time = static_cast<uint64_t>(ori_time * scale);
                    if (ori_time >= 3'000) new_time = static_cast<uint64_t>((ori_time - 3'000) * scale + 3'000);
                    else new_time = ori_time;
                    if (node.args.find("cpuinterval") != node.args.end()) { new_time += std::stoull(node.args["cpuinterval"]); }
                    node.args["time"] = std::to_string(new_time);
                }
                catch (...) {
                }
            }
        }
    }
}

// ======================================================================
//  simulation
// ======================================================================

void TraceDAG::simulation() {
    critical_pred.clear();
    std::unordered_map<size_t, std::vector<size_t>> temp_edge_dict;
    std::unordered_map<size_t, int> temp_node_incount;
    std::unordered_map<size_t, uint64_t> complete_time;

    std::unordered_map<size_t, std::unordered_map<size_t, DAGEdgeType>> edge_type_map;

    std::queue<size_t> Q;

    for (const auto & edge : edges) {
        temp_edge_dict[edge.src].push_back(edge.dst);
        temp_node_incount[edge.dst]++;
        edge_type_map[edge.src][edge.dst] = edge.type;
    }

    for (const auto & v : nodes) {
        if (temp_node_incount.count(v.id) == 0) {
            temp_node_incount[v.id] = 0;
            Q.push(v.id);
        }
        complete_time[v.id] = 0;
        critical_pred[v.id] = static_cast<size_t>(-1);
    }

    uint64_t e2e = 0;
    size_t count = 0;

    while (!Q.empty()) {
        count++;
        size_t u = Q.front();
        Q.pop();

        uint64_t node_time = 0;
        if (nodes[u].args.count("time")) {
            try {
                node_time = std::stoull(nodes[u].args["time"]);
            }
            catch (...) {
            }
        }
        complete_time[u] = node_time;

        if (critical_pred[u] != static_cast<size_t>(-1)) { complete_time[u] += complete_time[critical_pred[u]]; }

        if (nodes[u].args.find("cpuinterval") != nodes[u].args.end()) {
            size_t v = critical_pred[u];
            if (v != static_cast<size_t>(-1) && nodes[v].args.find("cpuinterval") != nodes[v].args.end()) {
                try {
                    if (std::stoull(nodes[v].args["cpuinterval"]) <= 1'000'000'000'000ull) { complete_time[u] += std::stoull(nodes[v].args["cpuinterval"]); }
                }
                catch (...) {
                }
            }
        }

        if (critical_pred[u] != static_cast<size_t>(-1) && nodes[u].args["tid"] == nodes[critical_pred[u]].args["tid"] && !cputid.count(nodes[u].args["tid"])
            && nodes[u].args.find("gpuid") != nodes[u].args.end()) {
            complete_time[u] += 1.2;
        }

        nodes[u].args["simulationtime"] = std::to_string(complete_time[u] - node_time);
        if (complete_time[u] > e2e) e2e = complete_time[u];

        for (size_t v : temp_edge_dict[u]) {
            uint64_t candidate_time = complete_time[u];
            if (critical_pred[v] == static_cast<size_t>(-1) || candidate_time > complete_time[critical_pred[v]]) { critical_pred[v] = u; }
            temp_node_incount[v]--;
            if (temp_node_incount[v] == 0) { Q.push(v); }
        }
    }

    // Cycle detection
    if (count < nodes.size()) {
        Logger::instance().error() << "Cycle detected in DAG! Processed " << count << " out of " << nodes.size() << " nodes.";

        std::unordered_map<size_t, int> visit_state;
        std::vector<size_t> path_stack;
        std::vector<size_t> cycle_path;

        std::function<bool(size_t)> dfs = [&](size_t u) -> bool {
            visit_state[u] = 1;
            path_stack.push_back(u);

            for (size_t v : temp_edge_dict[u]) {
                if (temp_node_incount[v] == 0) continue;

                if (visit_state[v] == 0) {
                    if (dfs(v)) return true;
                }
                else if (visit_state[v] == 1) {
                    auto it = std::find(path_stack.begin(), path_stack.end(), v);
                    if (it != path_stack.end()) { cycle_path.assign(it, path_stack.end()); }
                    return true;
                }
            }

            visit_state[u] = 2;
            path_stack.pop_back();
            return false;
        };

        for (const auto & pair : temp_node_incount) {
            if (pair.second > 0 && visit_state[pair.first] == 0) {
                if (dfs(pair.first)) { break; }
            }
        }

        if (!cycle_path.empty()) {
            auto l = Logger::instance().error();
            l << "\n========== EXACT CYCLE DETAILS ==========\n";
            for (size_t i = 0; i < cycle_path.size(); ++i) {
                size_t curr_node = cycle_path[i];
                size_t next_node = (i + 1 < cycle_path.size()) ? cycle_path[i + 1] : cycle_path[0];

                auto & rec = records[nodeid2recid[curr_node]];

                std::string conn_id = "N/A";
                if (rec->args.count("connection_id")) { conn_id = rec->args["connection_id"]; }

                DAGEdgeType edge_type = edge_type_map[curr_node][next_node];
                if (i >= 4 && i < cycle_path.size() - 4 && edge_type != DAGEdgeType::Sync) continue;
                l << "[Node] ID: " << curr_node << " | Name: " << rec->name << " | connection_id: " << conn_id << " | ts: " << rec->ts
                  << " | pid: " << rec->args["tid"] << " | dur: " << rec->dur << "\n"
                  << "   | \n"
                  << "   V (Edge Type: " << static_cast<int>(edge_type) << ")\n";
            }
            l << "[Node] ID: " << cycle_path[0] << " (Cycle completes here)\n";
            l << "=========================================";
        }

        throw std::runtime_error("Cycle detected in DAG. Simulation aborted.");
    }

    e2e_time_ = e2e;
    Logger::instance().info() << "Simulation completed. End-to-End time: " << e2e << " ns  |  nodes: " << count << "  edges: " << edges.size();
}

// ======================================================================
//  merge_graphs
// ======================================================================

TraceDAG TraceDAG::merge_graphs(std::vector<TraceDAG> & graphs) {
    if (graphs.empty()) return TraceDAG();
    if (graphs.size() == 1) return std::move(graphs[0]);

    TraceDAG merged_graph;
    std::vector<size_t> node_offsets(graphs.size() + 1, 0);
    std::unordered_map<std::string, std::unordered_map<int, std::vector<size_t>>> hccl_comm_groups;

    for (size_t gid = 0; gid < graphs.size(); ++gid) {
        auto & g = graphs[gid];
        size_t offset = node_offsets[gid];
        node_offsets[gid + 1] = offset + g.nodes.size();

        for (auto & node : g.nodes) {
            DAGNode new_node = node;
            new_node.id += offset;
            merged_graph.nodes.push_back(std::move(new_node));
        }

        for (auto & edge : g.edges) { merged_graph.edges.emplace_back(edge.src + offset, edge.dst + offset, edge.type); }

        for (auto & rec : g.records) { merged_graph.records.push_back(std::move(rec)); }
        for (auto recid : g.nodeid2recid) { merged_graph.nodeid2recid.push_back(recid + (offset == 0 ? 0 : merged_graph.records.size() - g.records.size())); }

        for (size_t local_node_id : g.hcclkernel2nodeid) {
            size_t global_node_id = local_node_id + offset;
            merged_graph.hcclkernel2nodeid.push_back(global_node_id);

            const auto & node = merged_graph.nodes[global_node_id];
            auto it = node.args.find("name");
            if (it != node.args.end()) { hccl_comm_groups[it->second][gid].push_back(global_node_id); }
        }

        for (const auto & tid : g.cputid) { merged_graph.cputid.insert(tid); }
    }

    for (const auto & kv_outer : hccl_comm_groups) {
        const auto & unique_id = kv_outer.first;
        const auto & gpu_comm_nodes = kv_outer.second;
        size_t max_comm_count = 0;
        for (const auto & kv_inner : gpu_comm_nodes) { max_comm_count = std::max(max_comm_count, kv_inner.second.size()); }

        for (size_t comm_idx = 0; comm_idx < max_comm_count; comm_idx++) {
            std::vector<std::pair<int, size_t>> curr_comm_nodes;
            for (const auto & kv_inner : gpu_comm_nodes) {
                int gpu_i = kv_inner.first;
                const auto & nodes = kv_inner.second;
                if (comm_idx < nodes.size()) { curr_comm_nodes.push_back({ gpu_i, nodes[comm_idx] }); }
            }

            uint64_t min_comm_time = 0;
            for (const auto & p : curr_comm_nodes) {
                int gpu_i = p.first;
                size_t node_i = p.second;
                auto & node_i_ref = merged_graph.nodes[node_i];
                uint64_t time = 0;
                if (node_i_ref.args.count("time")) time = std::stoull(node_i_ref.args["time"]);
                if (min_comm_time == 0 || time < min_comm_time) min_comm_time = time;

                auto next_it = node_i_ref.args.find("hccl_sync");
                if (next_it == node_i_ref.args.end()) continue;

                size_t next_node_local = std::stoull(next_it->second);
                size_t next_node_global = next_node_local + node_offsets[gpu_i];

                for (const auto & p2 : curr_comm_nodes) {
                    if (gpu_i == p2.first && node_i == p2.second) continue;
                    merged_graph.edges.emplace_back(p2.second, next_node_global, DAGEdgeType::HCCL);
                }
            }
            for (const auto & p : curr_comm_nodes) { merged_graph.nodes[p.second].args["time"] = std::to_string(min_comm_time); }
        }
    }

    return merged_graph;
}

// ======================================================================
//  to_chrome_tracing_json
// ======================================================================

void TraceDAG::to_chrome_tracing_json(const std::string & filename, bool concise, bool full_output) {
    std::ofstream ofs(filename);
    ofs << "{\n  \"traceEvents\": [\n";

    uint64_t real_min = 0;
    uint64_t real_max = 0;

    for (const auto & v : nodes) {
        if (v.id < nodeid2recid.size()) {
            int recid = nodeid2recid[v.id];
            auto ts = records[recid]->ts;
            if (real_min == 0 || ts < real_min) real_min = ts;
            if (ts > real_max) real_max = ts;
        }
    }
    Logger::instance().info() << "Real End-to-End time: " << real_max - real_min << " ns";

    if (!full_output) {
        ofs << "\n  ]\n}\n";
        return;
    }

    bool first = true;
    for (const auto & node : nodes) {
        if (node.id >= nodeid2recid.size()) continue;

        auto & rec = records[nodeid2recid[node.id]];

        std::string final_pid = "-1";
        std::string final_tid = "-1";

        if (rec->args.count("processId")) final_pid = rec->args.at("processId");
        else if (rec->args.count("deviceId")) final_pid = rec->args.at("deviceId");
        else if (rec->args.count("pid")) final_pid = rec->args.at("pid");

        if (rec->args.count("threadId")) final_tid = rec->args.at("threadId");
        else if (rec->args.count("streamId")) final_tid = rec->args.at("streamId");
        else if (rec->args.count("tid")) final_tid = rec->args.at("tid");

        std::string args_str = "{";
        if (!concise) {
            std::unordered_map<std::string, std::string> merged_args = rec->args;
            for (const auto & kv : node.args) { merged_args[kv.first] = kv.second; }
            bool first_arg = true;
            for (const auto & kv : merged_args) {
                if (kv.first == "tid" || kv.first == "gpuid" || kv.first == "pid" || kv.first == "processId" || kv.first == "deviceId" || kv.first == "threadId"
                    || kv.first == "streamId" || kv.first == "sim_pid" || kv.first == "sim_tid")
                    continue;

                if (!first_arg) args_str += ", ";
                args_str += "\"" + ActivityRecord::escape_json(kv.first) + "\": ";

                bool is_strict_num = false;
                if (!kv.second.empty() && (std::isdigit(kv.second[0]) || kv.second[0] == '-' || kv.second[0] == '+')) {
                    try {
                        size_t pos = 0;
                        std::stod(kv.second, &pos);
                        if (pos == kv.second.length()) is_strict_num = true;
                    }
                    catch (...) {
                    }
                }

                if (is_strict_num) args_str += kv.second;
                else args_str += "\"" + ActivityRecord::escape_json(kv.second) + "\"";
                first_arg = false;
            }
        }
        args_str += "}";

        uint64_t sim_time = rec->ts;
        if (sim_time == 0) continue;
        if (node.args.count("simulationtime")) {
            try {
                sim_time = std::stoull(node.args.at("simulationtime")) + real_min;
            }
            catch (...) {
            }
        }
        uint64_t dur = rec->dur;
        if (node.args.count("time")) {
            try {
                dur = std::stoull(node.args.at("time"));
            }
            catch (...) {
            }
        }

        if (!first) ofs << ",\n";
        first = false;

        // Real timeline event
        ofs << "    {\n"
            << "      \"name\": \"node_" << ActivityRecord::escape_json(rec->name) << "\",\n"
            << "      \"cat\": \"node_" << ActivityRecord::escape_json(rec->cat) << "\",\n"
            << "      \"ph\": \"X\",\n"
            << "      \"ts\": " << rec->ts << ",\n"
            << "      \"dur\": " << rec->dur << ",\n"
            << "      \"pid\": " << final_pid << "000,\n"
            << "      \"tid\": " << final_tid << ",\n"
            << "      \"args\": " << args_str << "\n"
            << "    },\n";

        // Simulation timeline event
        std::string sim_pid = final_pid;
        std::string sim_tid = final_tid;

        if (node.args.count("sim_pid")) sim_pid = node.args.at("sim_pid");
        else if (node.args.count("gpuid")) sim_pid = node.args.at("gpuid");

        if (node.args.count("sim_tid")) sim_tid = node.args.at("sim_tid");
        else if (node.args.count("tid")) sim_tid = node.args.at("tid");

        ofs << "    {\n"
            << "      \"name\": \"node_" << ActivityRecord::escape_json(rec->name) << "\",\n"
            << "      \"cat\": \"node_" << ActivityRecord::escape_json(rec->cat) << "\",\n"
            << "      \"ph\": \"X\",\n"
            << "      \"ts\": " << sim_time << ",\n"
            << "      \"dur\": " << dur << ",\n"
            << "      \"pid\": " << sim_pid << ",\n"
            << "      \"tid\": " << sim_tid << ",\n"
            << "      \"args\": " << args_str << "\n"
            << "    }";
    }

    // Edge flow events
    for (const auto & edge : edges) {
        if (edge.src >= nodeid2recid.size() || edge.dst >= nodeid2recid.size()) continue;

        auto & rec1 = records[nodeid2recid[edge.src]];
        auto & rec2 = records[nodeid2recid[edge.dst]];

        std::string pid1 = "-1", tid1 = "-1", pid2 = "-1", tid2 = "-1";

        if (rec1->args.count("processId")) pid1 = rec1->args.at("processId");
        else if (rec1->args.count("deviceId")) pid1 = rec1->args.at("deviceId");
        else if (rec1->args.count("pid")) pid1 = rec1->args.at("pid");
        if (rec1->args.count("threadId")) tid1 = rec1->args.at("threadId");
        else if (rec1->args.count("streamId")) tid1 = rec1->args.at("streamId");
        else if (rec1->args.count("tid")) tid1 = rec1->args.at("tid");

        if (nodes[edge.src].args.count("sim_pid")) pid1 = nodes[edge.src].args.at("sim_pid");
        else if (nodes[edge.src].args.count("gpuid")) pid1 = nodes[edge.src].args.at("gpuid");
        if (nodes[edge.src].args.count("sim_tid")) tid1 = nodes[edge.src].args.at("sim_tid");
        else if (nodes[edge.src].args.count("tid")) tid1 = nodes[edge.src].args.at("tid");

        if (rec2->args.count("processId")) pid2 = rec2->args.at("processId");
        else if (rec2->args.count("deviceId")) pid2 = rec2->args.at("deviceId");
        else if (rec2->args.count("pid")) pid2 = rec2->args.at("pid");
        if (rec2->args.count("threadId")) tid2 = rec2->args.at("threadId");
        else if (rec2->args.count("streamId")) tid2 = rec2->args.at("streamId");
        else if (rec2->args.count("tid")) tid2 = rec2->args.at("tid");

        if (nodes[edge.dst].args.count("sim_pid")) pid2 = nodes[edge.dst].args.at("sim_pid");
        else if (nodes[edge.dst].args.count("gpuid")) pid2 = nodes[edge.dst].args.at("gpuid");
        if (nodes[edge.dst].args.count("sim_tid")) tid2 = nodes[edge.dst].args.at("sim_tid");
        else if (nodes[edge.dst].args.count("tid")) tid2 = nodes[edge.dst].args.at("tid");

        uint64_t time1 = real_min, time2 = real_min;
        if (nodes[edge.src].args.count("simulationtime")) time1 += std::stoull(nodes[edge.src].args.at("simulationtime"));
        if (nodes[edge.dst].args.count("simulationtime")) time2 += std::stoull(nodes[edge.dst].args.at("simulationtime"));

        std::stringstream ss;
        ss << "0x" << std::hex << edge.src << edge.dst;

        ofs << ",\n    {\n"
            << "      \"name\": \"edge\",\n"
            << "      \"cat\": \"edge\",\n"
            << "      \"ph\": \"s\",\n"
            << "      \"ts\": " << time1 << ",\n"
            << "      \"pid\": " << pid1 << ",\n"
            << "      \"tid\": " << tid1 << ",\n"
            << "      \"id\": \"" << ss.str() << "\",\n"
            << "      \"args\": {\"type\": \"" << static_cast<int>(edge.type) << "\"}\n"
            << "    },\n"
            << "    {\n"
            << "      \"name\": \"edge\",\n"
            << "      \"cat\": \"edge\",\n"
            << "      \"ph\": \"t\",\n"
            << "      \"ts\": " << time2 << ",\n"
            << "      \"pid\": " << pid2 << ",\n"
            << "      \"tid\": " << tid2 << ",\n"
            << "      \"id\": \"" << ss.str() << "\",\n"
            << "      \"args\": {\"type\": \"" << static_cast<int>(edge.type) << "\"}\n"
            << "    }";
    }
    ofs << "\n  ]\n}\n";
}

} // namespace TraceGraph
