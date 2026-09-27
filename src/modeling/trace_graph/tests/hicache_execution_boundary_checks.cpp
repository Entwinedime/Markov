#include "markov/trace_graph/core/cpu_gap_observation.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/patch/source_dag_index.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include <stdexcept>

using namespace markov::trace_graph;
namespace hc = modules::hicache;
namespace {
void require(bool value, const char * message) {
    if (!value) throw std::runtime_error(message);
}
core::TraceEvent event(const char * name, uint64_t at, uint64_t duration) {
    core::TraceEvent e;
    e.name = name;
    e.pid = e.tid = "main";
    e.ts = at;
    e.dur = duration;
    return e;
}
core::TraceEvent commit(size_t id, uint64_t at) {
    auto e = event("hicache_commit", at, 0);
    e.index = id;
    e.source_channel = core::TraceSourceChannel::PythonProbe;
    e.set_arg("fact", R"({"class":"workload_identity","role":"cache_lifecycle_commit","consumers":["hicache_state_model"]})");
    e.set_arg("phase", "end");
    e.set_arg("request_id", std::to_string(id));
    e.set_arg("seq_no", std::to_string(id + 1));
    e.set_arg("cache_scope", "cache");
    e.set_arg("lifecycle_kind", "finished");
    e.set_arg("token_count", "4");
    e.set_arg("full_path_span", R"({"path_id":"p","begin":0,"end":4,"token_count":4})");
    e.set_arg("token_dictionary", R"({"path_id":"p","token_ids":[1,2,3,4]})");
    return e;
}
core::DagGraph graph() { return core::DagBuilder(1).build({ event("before", 100, 10), event("after", 200, 10) }, 3); }
void batch_points() {
    for (const bool interval : {false, true}) {
        auto measured = graph();
        measured.cpu_service_cost().add({"main", "main"}, {110, 200, 45});
        const auto baseline = simulation::run_topological_simulation(measured);
        const auto original = measured.nodes();
        auto observation = event("measured boundary", 140, interval ? 20 : 0);
        require(core::insert_cpu_gap_observation(measured, observation).has_value(), "measured gap can be partitioned");
        require(simulation::run_topological_simulation(measured).e2e_us == baseline.e2e_us,
                "observed boundary insertion must preserve measured CPU service, not restore raw wall gaps");
        for (const auto & node : original)
            require(measured.node(node.id).completion_time == node.completion_time,
                    "measured gap partition must preserve original completion times");
        auto later = event("later measured boundary", 180, 0);
        require(core::insert_cpu_gap_points(measured, std::span(&later, 1))[0].has_value(),
                "a measured synthetic suffix supports another boundary");
        require(simulation::run_topological_simulation(measured).e2e_us == baseline.e2e_us,
                "repeated boundary splits conserve the same CPU budget");
        const auto modeled = measured.add_synthetic_node({.name = "modeled gap", .cpu_gap_after = 20,
            .observed_point = core::DagObservedPoint{"main", "main", 140, 3}});
        require(measured.cpu_service_gap_duration(modeled) == 20,
                "newly modeled gaps must not consume original CPU budgets a second time");
    }
    auto g = graph();
    // A merged graph owns gpu_id=0, but its input nodes still belong to rank 3.
    std::vector<core::DagGraph> inputs;
    inputs.push_back(std::move(g));
    g = core::DagGraph::merge(std::move(inputs));
    const auto fork = g.add_synthetic_node({ .name = "independent consumer", .duration = 20, .counts_toward_e2e = true });
    g.add_edge(0, fork, core::DagEdgeKind::Mutation);
    const auto baseline = simulation::run_topological_simulation(g);
    const auto old_nodes = g.nodes();
    std::vector<core::TraceEvent> points{ event("point", 180, 0),
                                          event("point", 140, 0),
                                          event("same instant", 140, 0),
                                          event("inside leaf", 105, 0),
                                          event("before lane", 90, 0) };
    auto missing = event("no lane", 150, 0);
    missing.tid = "missing";
    points.push_back(missing);
    points.push_back(event("interval is not a point", 150, 1));
    const auto bound = core::insert_cpu_gap_points(g, points);
    require(bound[0] && bound[1] && bound[1] == bound[2], "same-gap points sort by time; equal points share one boundary");
    for (size_t i = 3; i < bound.size(); ++i) require(!bound[i], "unsupported point must stay unbound");
    require(g.node_count() == old_nodes.size() + 2, "only the two distinct valid points are inserted");
    require(simulation::run_topological_simulation(g).e2e_us == baseline.e2e_us, "batch insertion preserves full replay time");
    for (const auto & node : old_nodes)
        require(g.node(node.id).simulation_start == node.simulation_start && g.node(node.id).completion_time == node.completion_time,
                "all original nodes, including non-sequential consumers, must retain their times");
    for (size_t i = 0; i < 2; ++i)
        require(g.node(*bound[i]).gpu_id == 3 && !g.node(*bound[i]).counts_toward_e2e && g.node(*bound[i]).duration == 0,
                "points inherit rank, not cost or HTTP endpoint eligibility");
    for (const bool batch : { false, true })
        for (const uint64_t cost : { 0, 5, 25 }) {
            auto modeled = graph();
            modeled.set_node_duration(0, cost);
            modeled.set_node_duration(1, 40);
            const auto device = modeled.add_synthetic_node({ .name = "device readiness", .is_cpu = false, .duration = 75 });
            modeled.add_edge(device, 0, core::DagEdgeKind::Sync);
            const auto side = modeled.add_synthetic_node({ .name = "independent consumer", .duration = 8 });
            modeled.add_edge(0, side, core::DagEdgeKind::Mutation);
            (void)simulation::run_topological_simulation(modeled);
            const auto original = modeled.nodes();
            const auto point = batch ? core::insert_cpu_gap_points(modeled, std::span(points).subspan(1, 1))[0]
                                     : std::optional{ core::insert_cpu_gap_observation(modeled, points[1]).value().begin };
            require(point.has_value(), "changed adjacent service costs do not change ownership of an untouched gap");
            (void)simulation::run_topological_simulation(modeled);
            for (const auto & old : original)
                require(modeled.node(old.id).simulation_start == old.simulation_start && modeled.node(old.id).completion_time == old.completion_time,
                        "gap insertion preserves modeled costs, device waits and independent consumers");
            require(modeled.node(*point).completion_time == original[0].completion_time + 30,
                    "the observed 30us post-return offset follows modeled completion, not absolute source time");
        }
    const auto reject = [&](core::DagGraph candidate) {
        const auto n = candidate.node_count(), e = candidate.edge_count();
        require(!core::insert_cpu_gap_points(candidate, std::span(points).subspan(1, 1))[0], "unsupported gap is not split");
        require(candidate.node_count() == n && candidate.edge_count() == e, "rejection must leave the graph unchanged");
    };
    auto changed = graph();
    changed.mutable_node(0).cpu_gap_after -= 1;
    reject(changed);
    auto scoped = graph();
    scoped.add_scope_gap_duration(0, 1);
    reject(scoped);
    auto branch = graph();
    const auto id = branch.add_synthetic_node({ .name = "extra consumer" });
    branch.add_edge(0, id, core::DagEdgeKind::Sequential);
    reject(branch);
    auto worker = graph();
    const auto submit = worker.add_synthetic_node({ .name = "submission", .lane_key = "other" });
    worker.add_edge(submit, 0, core::DagEdgeKind::Correlation);
    reject(worker);
    auto explicit_task = graph();
    explicit_task.mutable_node(0).explicit_cpu_task = true;
    reject(explicit_task);
    // Two distinct sequential intervals covering the same observed pid/tid are ambiguous.
    auto overlap = graph();
    overlap.mutable_events().push_back(event("other before", 100, 10));
    const auto a = overlap.add_node(overlap.events().size() - 1, true, "other lane");
    overlap.mutable_events().push_back(event("other after", 200, 10));
    const auto b = overlap.add_node(overlap.events().size() - 1, true, "other lane");
    overlap.mutable_node(a).cpu_gap_after = 90;
    overlap.add_edge(a, b, core::DagEdgeKind::Sequential);
    reject(overlap);
}
void terminal_points() {
    auto g = graph();
    const auto side = g.add_synthetic_node({ .name = "independent consumer", .duration = 5, .counts_toward_e2e = true });
    g.add_edge(1, side, core::DagEdgeKind::Mutation);
    const auto baseline = simulation::run_topological_simulation(g);
    const auto old = g.nodes();
    const std::vector<core::TraceEvent> points{ event("tail", 240, 0), event("tail", 220, 0), event("same tail", 220, 0) };
    const auto bound = core::insert_cpu_gap_points(g, points);
    require(bound[0] && bound[1] && bound[1] == bound[2], "window-clipped lane permits observed tail points, sorted and deduplicated");
    require(simulation::run_topological_simulation(g).e2e_us == baseline.e2e_us, "tail state points do not extend the business endpoint");
    for (const auto & node : old)
        require(g.node(node.id).simulation_start == node.simulation_start && g.node(node.id).completion_time == node.completion_time,
                "extending a proven tail must preserve every original execution time");
    require(g.node(*bound[0]).completion_time == 140 && g.node(*bound[1]).completion_time == 120, "tail points retain observed delays from lane origin");
    g.set_node_duration(0, 60);
    (void)simulation::run_topological_simulation(g);
    require(g.node(*bound[0]).completion_time == 190 && g.node(*bound[1]).completion_time == 170,
            "tail facts move with preceding work, not absolute source timestamps");
    const auto reject = [&](core::DagGraph candidate) {
        require(!core::insert_cpu_gap_points(candidate, std::span(points).first(1))[0], "unproven tail cannot be attached to a nearby node");
    };
    auto changed = graph();
    changed.mutable_node(1).duration += 1;
    reject(changed);
    auto scoped = graph();
    scoped.mutable_node(1).cpu_gap_after = scoped.mutable_node(1).original_cpu_gap_after = 1;
    scoped.add_scope_gap_duration(1, 1);
    reject(scoped);
    auto delayed = graph();
    delayed.mutable_node(1).cpu_gap_after = 5;
    reject(delayed);
    auto task = graph();
    task.mutable_node(1).explicit_cpu_task = true;
    reject(task);
    auto worker = graph();
    const auto submit = worker.add_synthetic_node({ .name = "submit", .lane_key = "other" });
    worker.add_edge(submit, 1, core::DagEdgeKind::Correlation);
    reject(worker);
    auto ambiguous = graph();
    ambiguous.mutable_events().push_back(event("another tail", 200, 10));
    (void)ambiguous.add_node(ambiguous.events().size() - 1, true, "other lane");
    reject(ambiguous);
    auto tied = graph();
    tied.mutable_events().push_back(event("same end", 205, 5));
    (void)tied.add_node(tied.events().size() - 1, true, std::string(tied.node_lane_key(1)));
    reject(tied);
}
void fact_binding() {
    auto terminal = graph();
    terminal.set_hicache_fact_events({ commit(1, 220) });
    const auto terminal_binding = hc::bind_hicache_execution_boundaries(terminal);
    const auto terminal_nodes = terminal.node_count();
    require(terminal_binding.issues.empty() && terminal_binding.fact_nodes.size() == 1,
            "formal commit after the last retained CPU call must bind to its observed tail");
    require(hc::bind_hicache_execution_boundaries(terminal).fact_nodes == terminal_binding.fact_nodes && terminal.node_count() == terminal_nodes,
            "terminal fact binding must be idempotent");
    auto g = graph();
    auto first = commit(10, 140), same = commit(11, 140), tail = commit(7, 180);
    auto outcome = commit(12, 160);
    outcome.set_arg("fact", R"({"class":"source_actual","role":"cache_lifecycle_commit","consumers":["hicache_dag_patch"]})");
    g.set_hicache_fact_events({ first, same, outcome });
    auto unrelated = outcome;
    unrelated.index = 2;
    g.set_tail_context_events({ unrelated, tail });
    g.set_prelude_context_events({ commit(1, 90) });
    const hc::patch::HiCacheSourceDagIndex before(g);
    require(before.tail_context_facts().size() == 2 && before.tail_context_facts().back().node_id == 20,
            "tail identity uses original event index after the formal fact ID range, not filtered row or graph size");
    const auto bound = hc::bind_hicache_execution_boundaries(g);
    require(bound.issues.empty() && bound.fact_nodes.size() == 3 && bound.fact_nodes.at(10) == bound.fact_nodes.at(11),
            "router-approved formal and tail facts bind; same-time facts retain separate identities");
    require(!bound.fact_nodes.contains(12), "source actual outcomes must never become target state inputs");
    const auto nodes = g.node_count(), edges = g.edge_count();
    const auto again = hc::bind_hicache_execution_boundaries(g);
    require(again.fact_nodes == bound.fact_nodes && again.issues.empty() && g.node_count() == nodes && g.edge_count() == edges,
            "repeated binding must reuse exact state points without accumulating graph edits");
    require(hc::patch::HiCacheSourceDagIndex(g).tail_context_facts().back().node_id == 20, "inserting nodes cannot rename tail facts");
    auto missing = graph();
    auto f = commit(1, 140);
    f.tid = "no worker";
    missing.set_hicache_fact_events({ f });
    const auto unbound = hc::bind_hicache_execution_boundaries(missing);
    require(unbound.fact_nodes.empty() && unbound.issues.contains(1), "missing CPU lane must remain an explicit binding failure");
    auto invalid = graph();
    f = commit(1, 140);
    f.set_arg("seq_no", "0");
    invalid.set_hicache_fact_events({ f });
    const auto malformed = hc::bind_hicache_execution_boundaries(invalid);
    require(malformed.fact_nodes.empty() && malformed.issues.at(1) == std::vector<std::string>{ "missing_seq_no" } && invalid.node_count() == 2,
            "the existing router's required fields must be checked before inserting a state point");
    auto batch = commit(1, 140);
    batch.set_arg("fact", R"({"class":"workload_identity","role":"cache_extend_input","consumers":["hicache_state_model"]})");
    batch.set_arg("phase", "start");
    batch.set_arg("batch_kind", "extend");
    batch.set_arg("batch_size", "1");
    batch.set_arg("request_ids", R"(["r"])");
    batch.set_arg("token_dictionaries", R"([{"path_id":"p","token_ids":[1,2,3,4]}])");
    batch.set_arg("full_path_spans", R"([{"path_id":"p","begin":0,"end":4,"token_count":4}])");
    batch.set_arg("token_counts", "[4]");
    for (const bool valid : { true, false }) {
        auto target = graph();
        batch.set_arg("request_positions", valid ? R"([{"index":0,"request_id":"r"}])" : "{}");
        target.set_hicache_fact_events({ batch });
        const auto result = hc::bind_hicache_execution_boundaries(target);
        if (valid) require(result.issues.empty() && result.fact_nodes.contains(1), "valid parsed batch binds its state boundary");
        else
            require(result.fact_nodes.empty() && target.node_count() == 2
                        && result.issues.at(1)
                               == std::vector<std::string>{ "request_positions_not_array",
                                                            "request_positions_length_mismatch",
                                                            "request_positions_coverage",
                                                            "request_positions_request_id_mismatch" },
                    "batch parsing errors must reach admission unchanged and prevent DAG mutation");
    }
}
} // namespace

void check_hicache_execution_boundaries() {
    {
        hc::HiCacheFactParser parser;
        auto observed = commit(1, 10);
        require(!parser.parse(1, observed).chunked.has_value(), "legacy missing chunked stays unknown");
        observed.set_arg("chunked", "true");
        require(parser.parse(1, observed).chunked == true, "true chunked is preserved");
        observed.set_arg("chunked", "false");
        require(parser.parse(1, observed).chunked == false, "false chunked is distinct from missing");
        observed.set_arg("chunked", "maybe");
        bool rejected = false;
        try { (void)parser.parse(1, observed); }
        catch (const std::runtime_error &) { rejected = true; }
        require(rejected, "invalid chunked observation is not interpreted as false");
    }
    batch_points();
    terminal_points();
    fact_binding();
}
