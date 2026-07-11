/**
 * @file
 * @brief Serializes compact graph, simulation, and module run results.
 */
#include "run_summary.hpp"

#include "file_output.hpp"

#include "markov/trace_graph/modules/hicache/dag_patch_module.hpp"
#include "markov/trace_graph/modules/hicache/hicache_module.hpp"

#include <nlohmann/json.hpp>

namespace markov::trace_graph::cli {

namespace {

using Json = nlohmann::json;

Json effect_intent_result(const modules::hicache::HiCacheModule & module) {
    const auto & catalog = module.effect_intents();
    Json result;
    result["status"] = catalog.status;
    result["intent_count"] = catalog.intents.size();
    result["patchable_count"] = catalog.patchable_count();
    result["not_patchable_count"] = catalog.not_patchable_count();
    result["deferred_count"] = catalog.deferred_count();
    result["counts_by_effect_type"] = catalog.counts_by_effect_type();
    result["missing_facts"] = catalog.missing_facts;
    result["not_patchable_reasons"] = catalog.not_patchable_reasons;
    result["byte_projection_available"] = catalog.byte_projection_available;
    result["kv_bytes_per_page"] = catalog.kv_bytes_per_page;
    result["byte_projection_source"] = catalog.byte_projection_source;
    return result;
}

Json dag_patch_result(const modules::hicache::HiCacheDagPatchModule & module) {
    const auto & result = module.result();
    Json summary{
        {              "status",                      result.status },
        {             "plan_id",                result.plan.plan_id },
        {      "mutation_count",      result.journal.records.size() },
        { "active_nodes_before", result.journal.active_nodes_before },
        {  "active_nodes_after",  result.journal.active_nodes_after },
        { "active_edges_before", result.journal.active_edges_before },
        {  "active_edges_after",  result.journal.active_edges_after },
    };
#ifdef DEBUG
    summary["topology_valid"] = result.topology.ok();
#endif
    return summary;
}

Json module_results(const std::vector<std::unique_ptr<modules::SimulationModule>> & modules) {
    Json results = Json::object();
    for (const auto & module : modules) {
        if (const auto * hicache = dynamic_cast<const modules::hicache::HiCacheModule *>(module.get())) {
            results["hicache"] = Json{
                { "effect_intents", effect_intent_result(*hicache) }
            };
        }
        else if (const auto * patch = dynamic_cast<const modules::hicache::HiCacheDagPatchModule *>(module.get())) {
            results["hicache_dag_patch"] = dag_patch_result(*patch);
        }
    }
    return results;
}

Json run_summary(const core::DagGraph & graph, const std::vector<std::unique_ptr<modules::SimulationModule>> & modules) {
    const auto stats = graph.summary_stats();
    Json root;
    root["schema"] = "markov.trace_graph.run_summary.v3";
    root["parsed_record_count"] = graph.parsed_record_count();
    root["simulated_e2e_us"] = graph.e2e_time();
#ifdef DEBUG
    root["real_e2e_us"] = graph.real_e2e_time();
#endif
    root["node_count"] = stats.active_node_count;
    root["trace_node_count"] = stats.active_trace_node_count;
    root["synthetic_node_count"] = stats.active_synthetic_node_count;
    root["edge_count"] = stats.active_edge_count;
    root["stored_node_count"] = graph.node_count();
    root["stored_edge_count"] = graph.edge_count();
    root["edge_counts_by_kind"] = stats.edge_counts_by_kind;
    root["module_results"] = module_results(modules);
    return root;
}

} // namespace

void write_run_summary(const std::string & filename, const core::DagGraph & graph, const std::vector<std::unique_ptr<modules::SimulationModule>> & modules) {
    write_json_file(filename, run_summary(graph, modules));
}

#ifdef DEBUG
void write_run_summary(const std::string & filename, const core::DagGraph & graph, const std::vector<std::unique_ptr<modules::SimulationModule>> & modules,
                       const nlohmann::json & stage_timings) {
    auto root = run_summary(graph, modules);
    root["stage_timings_ms"] = stage_timings;
    write_json_file(filename, root);
}
#endif

} // namespace markov::trace_graph::cli
