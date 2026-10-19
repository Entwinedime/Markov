#include "markov/workflow/cpu_sampling_catalog.hpp"

#include "markov/contracts/json_input.hpp"
#include "markov/graph/cpu_service_partition.hpp"
#include "markov/templates/templates.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <set>
#include <string>
#include <utility>

namespace markov {

CpuSamplingCatalog::CpuSamplingCatalog(std::filesystem::path output_directory) : output_directory_(std::filesystem::absolute(output_directory)) {}

nlohmann::json CpuSamplingCatalog::collect(std::span<const TemplateDefinition * const> definitions) {
    const ServingContract contract;
    std::set<nlohmann::json> services;
    for (const auto * definition : definitions) {
        CpuServicePartition partition(definition->graph.boundary_count());
        for (const auto & [point, boundary] : definition->cpu_boundaries) partition.name(point, &boundary);
        for (const auto & edge : definition->graph.dependencies()) partition.connect(edge.source, edge.target, edge.relation);
        for (std::size_t index = 0; index < definition->graph.connection_count(); ++index) {
            const auto edge = definition->graph.connection(index);
            if (edge.contract.relation != SourceRelation::ThreadOrder && edge.contract.relation != SourceRelation::IntervalExtent)
                partition.protect(edge.contract.local_boundary);
        }

        for (std::size_t index = 0; index < definition->graph.size(); ++index) {
            const auto node = definition->graph[index];
            if (node.kind == SourceSpanKind::CpuControl && !definition->observation(node).transfer)
                partition.add({ index, node.begin, node.end, 0, definition->rank });
            else if (node.kind == SourceSpanKind::HostWait) partition.wait(node.begin, node.end);
            else if (node.role() != TemplateNodeRole::Annotation) {
                partition.protect(node.begin);
                partition.protect(node.end);
            }
        }
        for (const auto & [kind, point] : definition->ports) partition.protect(point);

        // Only the named cuts and their execution context are needed here.
        // Sampling observes batch features; it never borrows source counts.
        for (const auto & unit : partition.units())
            services.insert(nlohmann::json{
                { "service",                unit.service.describe() },
                {    "step",  contract.block(definition->step).name },
                {   "phase", contract.phase_name(definition->phase) },
                {    "rank",                       definition->rank }
            });
    }
    return {
        {                "services",           services },
        { "source_definition_count", definitions.size() }
    };
}

std::filesystem::path CpuSamplingCatalog::publish(const TemplateBank & primary, std::span<const TemplateBank * const> donors,
                                                  std::span<const TemplateDefinition * const> operation_sources) {
    std::set<const TemplateDefinition *> definitions;
    std::set<const TemplateBank *> banks{ &primary };
    banks.insert(donors.begin(), donors.end());
    for (const auto * bank : banks)
        for (const auto & [key, source_definitions] : bank->source().definitions)
            for (const auto & definition : source_definitions) definitions.insert(&definition);
    definitions.insert(operation_sources.begin(), operation_sources.end());

    std::vector<const TemplateDefinition *> selected(definitions.begin(), definitions.end());
    const auto existing = std::ranges::find(catalogs_, selected, &Catalog::definitions);
    if (existing != catalogs_.end()) return existing->path;

    // Published catalogs remain immutable across prediction rounds. Complete
    // content comparison also allows unchanged intervals to reuse one asset.
    const auto catalog = collect(selected);
    std::filesystem::create_directories(output_directory_);
    std::filesystem::path path;
    for (std::size_t index = 0;; ++index) {
        path = output_directory_ / ("cpu-services_" + std::to_string(index) + ".json");
        if (std::filesystem::exists(path)) {
            if (input::read_json(path) == catalog) break;
            continue;
        }

        auto pending = path;
        pending += ".tmp";
        std::ofstream output;
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output.open(pending);
        output << catalog << '\n';
        output.close();
        std::filesystem::rename(pending, path);
        break;
    }
    catalogs_.push_back({ std::move(selected), path });
    return path;
}

} // namespace markov
