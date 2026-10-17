#include "markov/workflow/extraction_workflow.hpp"

#include "markov/adapters/template_adapter.hpp"
#include "markov/contracts/json_input.hpp"
#include "markov/templates/template_asset.hpp"
#include "markov/templates/templates.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace markov {

const TemplateAdapter * ExtractionWorkflow::adapter(std::string_view framework) const {
    const auto found = std::ranges::find_if(adapters_, [&](const TemplateAdapter * item) { return item->name() == framework; });
    return found == adapters_.end() ? nullptr : *found;
}

nlohmann::json ExtractionWorkflow::run(const std::filesystem::path & manifest_path) const {
    auto manifest = input::read_json(manifest_path);
    if (manifest.at("role") != "base") throw std::invalid_argument("template extraction requires a base manifest");
    const auto * producer = adapter(manifest.at("framework").get<std::string_view>());
    if (!producer)
        return {
            {       "base_manifest",   manifest_path.string() },
            {           "readiness", manifest.at("readiness") },
            { "template_production",          "not_supported" }
        };

    const auto work = work_directory_.empty() ? std::filesystem::absolute(manifest_path).parent_path() : work_directory_;
    std::filesystem::create_directories(work);
    auto report = producer->extract(manifest_path, std::move(manifest), work);
    if (!report.at("graph_coverage").at("acyclic").get<bool>()) return report;

    report.update(compile(manifest_path));
    return report;
}

nlohmann::json ExtractionWorkflow::compile(const std::filesystem::path & manifest_path) const {
    auto manifest = input::read_json(manifest_path);
    const auto * producer = adapter(manifest.at("framework").get<std::string_view>());
    if (!producer) throw std::invalid_argument("framework template production not implemented");

    // Explicit recompilation invalidates the previous reference before any
    // fallible work. Failure cannot leave an old asset advertised as rebuilt.
    if (manifest.erase("compiled_templates") != 0) publish_manifest(manifest_path, manifest);

    // Successful captures retain facts and the compiled asset. Recreate the
    // temporary graph only when an explicit recompilation needs it again.
    if (!manifest.contains("source_dag") || !manifest.contains("templates")) return run(manifest_path);

    const auto started = std::chrono::steady_clock::now();
    const auto source = producer->produce(manifest_path);
    const auto directory = output_directory_.empty() ? std::filesystem::absolute(manifest_path).parent_path() : output_directory_;
    std::filesystem::create_directories(directory);
    const auto asset_path = directory / "compiled_templates.cbor";
    TemplateAsset::write(asset_path, source);
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();

    const auto prepared = input::read_json(input::resolve_path(manifest_path, manifest.at("input")));
    const bool diagnostics = prepared.at("diagnostics").get<bool>();
    std::vector<std::filesystem::path> consumed;
    if (!diagnostics) {
        consumed = { input::resolve_path(manifest_path, manifest.at("source_graph")), input::resolve_path(manifest_path, manifest.at("templates")) };
        manifest.erase("source_graph");
        manifest.erase("source_dag");
        manifest.erase("templates");
    }

    // Publish the replacement before removing inputs no consumer needs now.
    // A failed compile leaves the facts and its intermediate graph available.
    manifest["compiled_templates"] = std::filesystem::relative(asset_path, manifest_path.parent_path()).string();
    publish_manifest(manifest_path, manifest);
    const auto owned =
        std::filesystem::absolute(work_directory_.empty() ? std::filesystem::absolute(manifest_path).parent_path() : work_directory_).lexically_normal();
    for (const auto & path : consumed) {
        // Imported intermediates remain owned by their source task. Dropping a
        // reference here must not delete another run's only retained input.
        if (std::filesystem::absolute(path).lexically_normal().parent_path() == owned) std::filesystem::remove(path);
    }

    return {
        {   "compiled_templates",                                                                        asset_path.string() },
        { "template_compilation", { { "wall_time_ns", elapsed }, { "asset_bytes", std::filesystem::file_size(asset_path) } } }
    };
}

void ExtractionWorkflow::publish_manifest(const std::filesystem::path & path, const nlohmann::json & manifest) const {
    auto pending = path;
    pending += ".tmp";
    std::ofstream output;
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.open(pending);
    output << manifest.dump(2) << '\n';
    output.close();
    std::filesystem::rename(pending, path);
}

} // namespace markov
