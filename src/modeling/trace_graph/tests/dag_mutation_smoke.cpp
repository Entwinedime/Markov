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
using markov::trace_graph::core::DagSetNodeDurationMutation;
using markov::trace_graph::core::DagSyntheticNodeMutation;
using markov::trace_graph::core::DagSyntheticNodeSpec;
using markov::trace_graph::core::TraceByteRange;
using markov::trace_graph::core::TraceEvent;

void expect(bool condition, const std::string & message) {
    if (!condition) throw std::runtime_error(message);
}

void expect_plan_rejected_atomically(DagGraph & graph, const DagMutationPlan & plan, const std::string & message) {
    std::vector<bool> node_activity;
    std::vector<uint64_t> node_durations;
    node_activity.reserve(graph.node_count());
    node_durations.reserve(graph.node_count());
    for (const auto & node : graph.nodes()) {
        node_activity.push_back(node.active);
        node_durations.push_back(node.duration);
    }
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
    for (size_t index = 0; index < node_durations.size(); ++index)
        expect(graph.node(index).duration == node_durations[index], "rejected plan changed node duration");
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

void check_duration_update_preserves_boundary() {
    auto graph = make_linear_graph();
    graph.mutable_node(1).cpu_gap_after = 7;
    DagMutationPlan plan{
        .plan_id = "duration_update",
        .effect_id = "effect:duration_update",
        .set_node_durations = {
            DagSetNodeDurationMutation{
                .node_id = 1,
                .duration = 0,
                .reason = "remove_owned_cost",
            },
        },
    };
    const auto result = markov::trace_graph::core::apply_dag_mutation_plan(graph, plan);
    expect(result.journal.records.size() == 1, "duration update did not produce one journal record");
    expect(result.journal.records[0].old_duration == 20 && result.journal.records[0].new_duration == 0, "duration update journal lost old or new cost");
    expect(graph.node(1).active && graph.node(1).duration == 0, "duration update changed boundary-node activity");
    expect(graph.node(1).cpu_gap_after == 7, "duration update discarded the retained CPU gap");
    const auto simulation = markov::trace_graph::simulation::run_topological_simulation(graph);
    expect(simulation.e2e_us == 47, "duration update did not preserve the boundary CPU gap");

    auto conflict_graph = make_linear_graph();
    DagMutationPlan conflict{
        .plan_id = "duration_disable_conflict",
        .set_node_durations = { DagSetNodeDurationMutation{ .node_id = 1, .duration = 0 } },
        .disable_nodes = { 1 },
    };
    expect_plan_rejected_atomically(conflict_graph, conflict, "duration update and node disable conflict was accepted");
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

void check_existing_edge_deduplication_paths() {
    {
        auto graph = make_linear_graph();
        graph.add_edge(0, 2, DagEdgeKind::Mutation, "effect:existing");
        DagMutationPlan plan{
            .plan_id = "deduplicate_existing_effect_edge",
            .add_edges = {
                DagAddEdgeMutation{
                    .src = DagNodeRef::existing(0),
                    .dst = DagNodeRef::existing(2),
                    .kind = DagEdgeKind::Mutation,
                    .effect_id = "effect:existing",
                },
            },
        };
        const auto result = markov::trace_graph::core::apply_dag_mutation_plan(graph, plan);
        expect(result.journal.records.empty(), "existing provenance edge was added twice");
        expect(graph.active_edge_count() == 3, "existing provenance edge deduplication changed edge count");
    }
    {
        auto graph = make_linear_graph();
        DagMutationPlan plan{
            .plan_id = "deduplicate_existing_plain_edge",
            .add_edges = {
                DagAddEdgeMutation{
                    .src = DagNodeRef::existing(0),
                    .dst = DagNodeRef::existing(1),
                    .kind = DagEdgeKind::Sequential,
                },
            },
        };
        const auto result = markov::trace_graph::core::apply_dag_mutation_plan(graph, plan);
        expect(result.journal.records.empty(), "existing edge without provenance was added twice");
        expect(graph.active_edge_count() == 2, "plain-edge fallback deduplication changed edge count");
    }
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
    check_duration_update_preserves_boundary();
    check_addition_to_disabled_node_is_atomic();
    check_conflicting_redirects_are_atomic();
    check_duplicate_addition_is_deduplicated();
    check_existing_edge_deduplication_paths();
    check_lazy_argument_merge();
    return 0;
}
