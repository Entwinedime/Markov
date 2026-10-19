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

    // Withdraw the previous asset before fallible production. A failed rebuild
    // must not advertise its predecessor as the result of the current rules.
    if (manifest.erase("compiled_templates") != 0) publish_manifest(manifest_path, manifest);

    const auto work = work_directory_.empty() ? std::filesystem::absolute(manifest_path).parent_path() : work_directory_;
    std::filesystem::create_directories(work);
    const auto started = std::chrono::steady_clock::now();
    auto produced = producer->produce(manifest_path, std::move(manifest), work);
    auto & current = produced.manifest;
    auto gaps = current.at("gaps");
    for (const auto & gap : current.at("extraction_gaps")) gaps.push_back(gap);
    nlohmann::json report{
        {  "base_manifest",       manifest_path.string() },
        {      "readiness",      current.at("readiness") },
        { "graph_coverage", current.at("graph_coverage") },
        {           "gaps",              std::move(gaps) }
    };

    if (produced.source) {
        const auto directory = output_directory_.empty() ? std::filesystem::absolute(manifest_path).parent_path() : output_directory_;
        std::filesystem::create_directories(directory);
        const auto asset_path = directory / "compiled_templates.cbor";
        TemplateAsset::write(asset_path, *produced.source);
        current["compiled_templates"] = std::filesystem::relative(asset_path, manifest_path.parent_path()).string();
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
        report["compiled_templates"] = asset_path.string();
        report["template_production"] = {
            { "wall_time_ns",                                elapsed },
            {  "asset_bytes", std::filesystem::file_size(asset_path) }
        };
    }

    // The one production owner publishes after binding and asset writing.
    // Normal runs never create temporary graph files that need later cleanup.
    publish_manifest(manifest_path, current);
    return report;
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
