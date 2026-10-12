#include "markov/workflow/extraction_workflow.hpp"

#include "markov/adapters/frameworks/sglang/bindings.hpp"
#include "markov/adapters/frameworks/sglang/template_bindings.hpp"
#include "markov/adapters/runtimes/ascend/source_extractor.hpp"
#include "markov/templates/template_asset.hpp"
#include "markov/templates/templates.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace markov {

nlohmann::json ExtractionWorkflow::run(const std::filesystem::path & manifest_path) const {
    std::ifstream stream(manifest_path);
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    auto manifest = nlohmann::json::parse(stream);
    if (manifest.at("framework") != "sglang") throw std::invalid_argument("source extraction currently implements the SGLang observer contract");

    // Framework bindings outlive Ascend extraction and its borrowed block declarations.
    const SGLangBindings bindings;
    auto report = AscendSourceExtractor(bindings).extract(manifest_path, std::move(manifest));
    if (!report.at("graph_coverage").at("acyclic").get<bool>()) return report;

    report.update(compile(manifest_path));
    return report;
}

nlohmann::json ExtractionWorkflow::compile(const std::filesystem::path & manifest_path) const {
    std::ifstream stream(manifest_path);
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    auto manifest = nlohmann::json::parse(stream);
    stream.close();

    // Explicit recompilation invalidates the previous reference before any
    // fallible work. Failure cannot leave an old asset advertised as rebuilt.
    if (manifest.erase("compiled_templates") != 0) publish_manifest(manifest_path, manifest);

    const auto started = std::chrono::steady_clock::now();
    const auto source = SGLangTemplateBindings::produce(manifest_path);
    const auto asset_path = manifest_path.parent_path() / "compiled_templates.cbor";
    TemplateAsset::write(asset_path, source);
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();

    manifest["compiled_templates"] = asset_path.filename().string();
    publish_manifest(manifest_path, manifest);

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
