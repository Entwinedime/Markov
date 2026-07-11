/**
 * @file
 * @brief Focused executable tests for atomic DAG mutation and output round trips.
 *
 * The test intentionally uses a standalone executable so the production library
 * remains independent of a unit-test framework. Any failed expectation throws and
 * produces a nonzero CTest result.
 */
#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/io/chrome_trace_io.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using markov::trace_graph::core::DagAddEdgeMutation;
using markov::trace_graph::core::DagEdgeKind;
using markov::trace_graph::core::DagGraph;
using markov::trace_graph::core::DagMutationPlan;
using markov::trace_graph::core::DagNodeKind;
using markov::trace_graph::core::DagNodeRef;
using markov::trace_graph::core::DagRedirectEdgeMutation;
using markov::trace_graph::core::DagSyntheticNodeMutation;
using markov::trace_graph::core::DagSyntheticNodeSpec;
using markov::trace_graph::core::TraceByteRange;
using markov::trace_graph::core::TraceEvent;

void expect(bool condition, const std::string & message) {
    if (!condition) throw std::runtime_error(message);
}

void expect_plan_rejected_atomically(DagGraph & graph, const DagMutationPlan & plan, const std::string & message) {
    std::vector<bool> node_activity;
    node_activity.reserve(graph.node_count());
    for (const auto & node : graph.nodes()) node_activity.push_back(node.active);
    std::vector<bool> edge_activity;
    edge_activity.reserve(graph.edge_count());
    for (const auto & edge : graph.edges()) edge_activity.push_back(edge.active);

    bool rejected = false;
    try {
        (void)markov::trace_graph::core::apply_dag_mutation_plan(graph, plan);
    }
    catch (const markov::trace_graph::core::DagMutationValidationError &) {
        rejected = true;
    }
    expect(rejected, message);
    expect(graph.node_count() == node_activity.size() && graph.edge_count() == edge_activity.size(), "rejected plan changed graph storage");
    for (size_t index = 0; index < node_activity.size(); ++index)
        expect(graph.node(index).active == node_activity[index], "rejected plan changed node activity");
    for (size_t index = 0; index < edge_activity.size(); ++index)
        expect(graph.edge(index).active == edge_activity[index], "rejected plan changed edge activity");
}

DagGraph make_linear_graph() {
    std::vector<TraceEvent> events;
    for (size_t index = 0; index < 3; ++index) {
        TraceEvent event;
        event.index = index;
        event.name = std::string(1, static_cast<char>('A' + index));
        event.cat = "smoke";
        event.ts = index * 10;
        event.dur = (index + 1) * 10;
        event.pid = "1";
        event.tid = "1";
        events.push_back(std::move(event));
    }
    DagGraph graph(std::move(events), 0);
    const auto node_a = graph.add_node(0, true, "cpu");
    const auto node_b = graph.add_node(1, true, "cpu");
    const auto node_c = graph.add_node(2, true, "cpu");
    graph.add_edge(node_a, node_b, DagEdgeKind::Sequential);
    graph.add_edge(node_b, node_c, DagEdgeKind::Sequential);
    return graph;
}

void check_empty_plan() {
    auto graph = make_linear_graph();
    const auto before = markov::trace_graph::simulation::run_topological_simulation(graph);
    const auto result = markov::trace_graph::core::apply_dag_mutation_plan(graph, DagMutationPlan{ .plan_id = "empty" });
    const auto after = markov::trace_graph::simulation::run_topological_simulation(graph);
    expect(result.journal.records.empty(), "empty plan wrote mutation records");
    expect(before.e2e_us == after.e2e_us, "empty plan changed E2E");
    expect(graph.active_node_count() == 3 && graph.active_edge_count() == 2, "empty plan changed active graph counts");
}

void check_collapse_and_replace() {
    auto graph = make_linear_graph();
    DagMutationPlan plan{
        .plan_id = "collapse",
        .effect_id = "effect:smoke",
        .reason = "phase1_smoke",
        .disable_nodes = { 1 },
        .synthetic_nodes = {
            DagSyntheticNodeMutation{
                .synthetic_id = "replacement",
                .node = DagSyntheticNodeSpec{
                    .name = "replacement_io",
                    .category = "hicache_io",
                    .is_cpu = true,
                    .lane_key = "synthetic_io",
                    .duration = 5,
                },
            },
        },
        .redirect_edges = {
            DagRedirectEdgeMutation{
                .edge_index = 0,
                .dst = DagNodeRef::synthetic("replacement"),
            },
            DagRedirectEdgeMutation{
                .edge_index = 1,
                .src = DagNodeRef::synthetic("replacement"),
            },
        },
    };
    const auto validation = markov::trace_graph::core::validate_dag_mutation_plan(graph, plan);
    expect(validation.ok(), "valid collapse plan was rejected");
    const auto result = markov::trace_graph::core::apply_dag_mutation_plan(graph, plan);
    const auto replacement = result.synthetic_node_ids.at("replacement");
    expect(!graph.node(1).active, "collapsed source node remained active");
    expect(graph.node(replacement).kind == DagNodeKind::Synthetic, "replacement node lacks synthetic identity");
    expect(graph.event_for_node(replacement).name == "replacement_io", "synthetic event identity is missing");
    expect(graph.active_node_count() == 3 && graph.active_edge_count() == 2, "collapse produced wrong active graph counts");
    const auto simulation = markov::trace_graph::simulation::run_topological_simulation(graph);
    expect(simulation.e2e_us == 45, "collapse replacement produced wrong E2E");

    const std::string output = "/tmp/trace_graph_dag_mutation_smoke.json";
    markov::trace_graph::io::write_chrome_trace_dag(output, graph);
    std::ifstream input(output);
    const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::remove(output.c_str());
    expect(json.find("node_replacement_io") != std::string::npos, "writer omitted active synthetic node");
    expect(json.find("node_B") == std::string::npos, "writer emitted disabled source node");
}

void check_invalid_and_cycle_reports() {
    auto graph = make_linear_graph();
    DagMutationPlan invalid{
        .plan_id = "invalid",
        .add_edges = {
            DagAddEdgeMutation{
                .src = DagNodeRef::existing(99),
                .dst = DagNodeRef::existing(0),
            },
        },
    };
    const auto invalid_report = markov::trace_graph::core::validate_dag_mutation_plan(graph, invalid);
    expect(!invalid_report.ok() && !invalid_report.issues.empty(), "invalid endpoint was not reported");
    bool invalid_apply_rejected = false;
    try {
        (void)markov::trace_graph::core::apply_dag_mutation_plan(graph, invalid);
    }
    catch (const markov::trace_graph::core::DagMutationValidationError & error) {
        invalid_apply_rejected = !error.report().issues.empty();
    }
    expect(invalid_apply_rejected, "invalid apply did not preserve the topology report");

    DagMutationPlan cycle{
        .plan_id = "cycle",
        .effect_id = "effect:cycle",
        .add_edges = {
            DagAddEdgeMutation{
                .src = DagNodeRef::existing(2),
                .dst = DagNodeRef::existing(0),
            },
        },
    };
    const auto cycle_report = markov::trace_graph::core::validate_dag_mutation_plan(graph, cycle);
    expect(!cycle_report.ok() && !cycle_report.cycle_nodes.empty(), "cycle witness was not reported");
}

void check_incident_edge_journal() {
    auto graph = make_linear_graph();
    DagMutationPlan plan{
        .plan_id = "disable_node",
        .effect_id = "effect:disable_node",
        .reason = "smoke",
        .disable_nodes = { 1 },
    };
    const auto result = markov::trace_graph::core::apply_dag_mutation_plan(graph, plan);
    expect(result.journal.records.size() == 3, "node tombstone did not journal its two incident edges");
    expect(graph.active_node_count() == 2 && graph.active_edge_count() == 0, "node tombstone left active incident edges");
}

void check_addition_to_disabled_node_is_atomic() {
    auto graph = make_linear_graph();
    DagMutationPlan plan{
        .plan_id = "invalid_disabled_endpoint",
        .effect_id = "effect:invalid_disabled_endpoint",
        .disable_nodes = { 1 },
        .add_edges = {
            DagAddEdgeMutation{ .src = DagNodeRef::existing(0), .dst = DagNodeRef::existing(1) },
        },
    };
    expect_plan_rejected_atomically(graph, plan, "edge addition to a disabled node was accepted");
}

void check_conflicting_redirects_are_atomic() {
    {
        auto graph = make_linear_graph();
        DagMutationPlan plan{
            .plan_id = "duplicate_redirect",
            .redirect_edges = {
                DagRedirectEdgeMutation{ .edge_index = 0, .dst = DagNodeRef::existing(2) },
                DagRedirectEdgeMutation{ .edge_index = 0, .dst = DagNodeRef::existing(2) },
            },
        };
        expect_plan_rejected_atomically(graph, plan, "duplicate redirects were accepted");
    }
    {
        auto graph = make_linear_graph();
        DagMutationPlan plan{
            .plan_id = "disable_redirect_conflict",
            .disable_edges = { 0 },
            .redirect_edges = { DagRedirectEdgeMutation{ .edge_index = 0, .dst = DagNodeRef::existing(2) } },
        };
        expect_plan_rejected_atomically(graph, plan, "disable and redirect of the same edge were accepted");
    }
    {
        auto graph = make_linear_graph();
        DagMutationPlan plan{
            .plan_id = "redirect_disabled_endpoint",
            .disable_nodes = { 1 },
            .redirect_edges = { DagRedirectEdgeMutation{ .edge_index = 0, .dst = DagNodeRef::existing(1) } },
        };
        expect_plan_rejected_atomically(graph, plan, "redirect to a disabled node was accepted");
    }
    {
        auto graph = make_linear_graph();
        graph.disable_edge(0);
        DagMutationPlan plan{
            .plan_id = "redirect_inactive_edge",
            .redirect_edges = { DagRedirectEdgeMutation{ .edge_index = 0, .dst = DagNodeRef::existing(2) } },
        };
        expect_plan_rejected_atomically(graph, plan, "redirect of an inactive edge was accepted");
    }
}

void check_duplicate_addition_is_deduplicated() {
    auto graph = make_linear_graph();
    DagMutationPlan plan{
        .plan_id = "deduplicate",
        .effect_id = "effect:deduplicate",
        .add_edges = {
            DagAddEdgeMutation{ .src = DagNodeRef::existing(0), .dst = DagNodeRef::existing(2) },
            DagAddEdgeMutation{ .src = DagNodeRef::existing(0), .dst = DagNodeRef::existing(2) },
        },
    };
    const auto result = markov::trace_graph::core::apply_dag_mutation_plan(graph, plan);
    (void)result;
    expect(markov::trace_graph::core::validate_active_dag(graph).ok(), "deduplicated graph is invalid");
    expect(graph.active_edge_count() == 3, "duplicate planned edge was not deduplicated");
}

void check_lazy_argument_merge() {
    auto target_buffer = std::make_shared<const std::string>(R"({"shared":"target","target_only":1})");
    auto source_buffer = std::make_shared<const std::string>(R"({"shared":"source","nested":{"value":7}})");

    TraceEvent target;
    target.pid = "target_pid";
    target.tid = "target_tid";
    target.set_args_json_slice(target_buffer, TraceByteRange{ .offset = 0, .length = target_buffer->size() });
    target.set_arg("frozen_override", "before_merge");

    TraceEvent source;
    source.pid = "source_pid";
    source.tid = "source_tid";
    source.set_args_json_slice(source_buffer, TraceByteRange{ .offset = 0, .length = source_buffer->size() });
    source.set_arg("source_override", "source_value");

    target.merge_args_from(source);
    expect(target.arg("shared") == "source", "merged raw arguments did not override the earlier source");
    expect(target.arg_u64("nested.value") == 7, "merged nested argument was not available lazily");
    expect(target.arg("frozen_override") == "before_merge", "pre-merge override was lost");
    expect(target.has_arg_override("frozen_override"), "frozen override was not discoverable");
    expect(target.arg("source_override") == "source_value", "source override was not merged");
    expect(target.arg("pid") == "target_pid", "argument merge changed the target event identity");

    target.set_arg("shared", "after_merge");
    expect(target.arg("shared") == "after_merge", "post-merge override did not take precedence");

    const TraceEvent copied = target;
    expect(copied.arg("target_only") == "1", "copy lost the target raw argument layer");
    expect(copied.arg_u64("nested.value") == 7, "copy lost the merged raw argument layer");
    expect(copied.args_map().at("shared") == "after_merge", "materialized merged arguments used the wrong precedence");
}

} // namespace

int main() {
    check_empty_plan();
    check_collapse_and_replace();
    check_invalid_and_cycle_reports();
    check_incident_edge_journal();
    check_addition_to_disabled_node_is_atomic();
    check_conflicting_redirects_are_atomic();
    check_duplicate_addition_is_deduplicated();
    check_lazy_argument_merge();
    return 0;
}
