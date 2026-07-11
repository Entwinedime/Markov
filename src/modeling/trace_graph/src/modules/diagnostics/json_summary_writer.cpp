/**
 * @file
 * @brief JSON serializer for SimulationModule diagnostics summaries.
 */
#include "markov/trace_graph/modules/diagnostics/json_summary_writer.hpp"

#include "markov/trace_graph/modules/hicache/dag_patch_module.hpp"
#include "markov/trace_graph/modules/hicache/diagnostics/summary.hpp"
#include "markov/trace_graph/modules/hicache/hicache_module.hpp"
#include "markov/trace_graph/modules/node_scale/node_scale_module.hpp"

#include <nlohmann/json.hpp>

namespace markov::trace_graph::modules::diagnostics {

namespace json_summary_writer_detail {

using Json = nlohmann::json;

Json node_scale_summary_json(const node_scale::NodeScaleModule & module) {
    const auto summary = module.summary();
    Json root{
        {         "name",           "NodeScaleModule" },
        {   "rule_count", summary.config.rules.size() },
        { "scaled_nodes",        summary.scaled_nodes },
    };
    root["rules"] = Json::array();
    for (const auto & rule : summary.config.rules) {
        root["rules"].push_back({
            {     "id",     rule.id },
            {   "name",   rule.name },
            { "factor", rule.factor },
        });
    }
    return root;
}

Json hicache_summary_json(const hicache::HiCacheModule & module) {
    return Json{
        { "name", "HiCacheModule" },
        { "hicache", Json::parse(hicache::diagnostics::summary_json(module.summary(), module.effect_intents())) },
    };
}

Json topology_report_json(const core::DagTopologyValidationReport & report) {
    Json issues = Json::array();
    for (const auto & issue : report.issues) {
        issues.push_back({
            {         "code",         issue.code },
            {      "message",      issue.message },
            {     "node_ids",     issue.node_ids },
            { "edge_indices", issue.edge_indices },
        });
    }
    return {
        {             "valid",              report.ok() },
        { "active_node_count", report.active_node_count },
        { "active_edge_count", report.active_edge_count },
        {       "cycle_nodes",       report.cycle_nodes },
        {            "issues",        std::move(issues) },
    };
}

Json optional_size_json(const std::optional<size_t> & value) { return value ? Json(*value) : Json(nullptr); }

Json patch_summary_json(const hicache::HiCacheDagPatchModule & module) {
    const auto & result = module.result();
    Json records = Json::array();
    for (const auto & record : result.journal.records) {
        records.push_back({
            {              "action",  core::dag_mutation_action_name(record.action) },
            {           "effect_id",                               record.effect_id },
            {              "reason",                                  record.reason },
            {             "node_id",             optional_size_json(record.node_id) },
            {          "edge_index",          optional_size_json(record.edge_index) },
            { "replaced_edge_index", optional_size_json(record.replaced_edge_index) },
            {                 "src",                 optional_size_json(record.src) },
            {                 "dst",                 optional_size_json(record.dst) },
        });
    }

    Json plan{
        {              "plan_id",                result.plan.plan_id },
        {            "effect_id",              result.plan.effect_id },
        {               "reason",                 result.plan.reason },
        {   "disable_node_count",   result.plan.disable_nodes.size() },
        {   "disable_edge_count",   result.plan.disable_edges.size() },
        { "synthetic_node_count", result.plan.synthetic_nodes.size() },
        {       "add_edge_count",       result.plan.add_edges.size() },
        {  "redirect_edge_count",  result.plan.redirect_edges.size() },
    };
    Json journal{
        {             "plan_id",             result.journal.plan_id },
        {      "mutation_count",      result.journal.records.size() },
        { "active_nodes_before", result.journal.active_nodes_before },
        {  "active_nodes_after",  result.journal.active_nodes_after },
        { "active_edges_before", result.journal.active_edges_before },
        {  "active_edges_after",  result.journal.active_edges_after },
        {             "records",                 std::move(records) },
    };
    return {
        {              "name","HiCacheDagPatchModule" },
        { "hicache_dag_patch",
         {
         { "status", result.status },
         { "plan", std::move(plan) },
         { "journal", std::move(journal) },
         { "topology", topology_report_json(result.topology) },
         }              },
    };
}

} // namespace json_summary_writer_detail

using json_summary_writer_detail::hicache_summary_json;
using json_summary_writer_detail::Json;
using json_summary_writer_detail::node_scale_summary_json;
using json_summary_writer_detail::patch_summary_json;

/**
 * @brief Serializes a diagnostics summary according to the module's concrete type.
 *
 * Summaries are explicit Debug/validation artifacts. Unknown modules retain a stable JSON
 * object so adding a business module does not break the CLI serialization path.
 */
std::string module_summary_json(const SimulationModule & module) {
    if (const auto * node_scale = dynamic_cast<const node_scale::NodeScaleModule *>(&module)) return node_scale_summary_json(*node_scale).dump();
    if (const auto * hicache = dynamic_cast<const hicache::HiCacheModule *>(&module)) return hicache_summary_json(*hicache).dump();
    if (const auto * patch = dynamic_cast<const hicache::HiCacheDagPatchModule *>(&module)) return patch_summary_json(*patch).dump();

    return Json{
        {           "name",             module.name() },
        { "summary_status", "unsupported_module_type" },
    }
        .dump();
}

} // namespace markov::trace_graph::modules::diagnostics
