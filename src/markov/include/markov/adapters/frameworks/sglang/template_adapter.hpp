#pragma once

#include "markov/adapters/template_adapter.hpp"

#include <nlohmann/json_fwd.hpp>

#include <expected>
#include <span>
#include <string_view>

namespace markov {

// Bind the installed framework/runtime to common templates and source planning.
// Grouping and recipes do not prove coverage or run collection.
class SGLangTemplateAdapter final : public TemplateAdapter {
public:
    Framework framework() const override { return Framework::SGLang; }
    std::string_view name() const override { return "sglang"; }
    nlohmann::json extract(const std::filesystem::path & manifest_path, nlohmann::json manifest, const std::filesystem::path & work_directory) const override;
    TemplateSource produce(const std::filesystem::path & manifest_path) const override;
    FrameworkConfiguration source_configuration(const nlohmann::json & parameters) const override;
    bool shares_execution_path(const FrameworkConfiguration & source, const FrameworkConfiguration & target) const override;
    std::expected<ServingLimits, std::string_view> control_limits(const FrameworkConfiguration & configuration) const override;

    nlohmann::json configurations(std::span<const Candidate * const> candidates) const override;
    TemplateSourceSelection select_sources(const Candidate & candidate, std::span<const TemplateSourceCandidate> sources) const override;
    nlohmann::json describe_sources(const TemplateSourceSelection & selection) const override;

private:
    nlohmann::json candidate_recipe(const SGLangConfiguration & configuration) const;
};

} // namespace markov
