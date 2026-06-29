/**
 * @file
 * @brief SimulationModule summary 的 JSON 写出器。
 */
#include "markov/trace_graph/modules/diagnostics/json_summary_writer.hpp"

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
        {    "name",                                                   "HiCacheModule" },
        { "hicache", Json::parse(hicache::diagnostics::summary_json(module.summary())) },
    };
}

} // namespace json_summary_writer_detail

using json_summary_writer_detail::hicache_summary_json;
using json_summary_writer_detail::Json;
using json_summary_writer_detail::node_scale_summary_json;

std::string module_summary_json(const SimulationModule & module) {
    if (const auto * node_scale = dynamic_cast<const node_scale::NodeScaleModule *>(&module)) return node_scale_summary_json(*node_scale).dump();
    if (const auto * hicache = dynamic_cast<const hicache::HiCacheModule *>(&module)) return hicache_summary_json(*hicache).dump();

    return Json{
        {           "name",             module.name() },
        { "summary_status", "unsupported_module_type" },
    }
        .dump();
}

} // namespace markov::trace_graph::modules::diagnostics
