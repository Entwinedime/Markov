#pragma once

#include "markov/adapters/configuration.hpp"
#include "markov/contracts/control.hpp"
#include "markov/templates/templates.hpp"

#include <nlohmann/json_fwd.hpp>

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace markov {

// The workflow owns the bank and parsed configuration throughout prediction.
struct TemplateSourceCandidate {
    const TemplateBank * bank;
    const FrameworkConfiguration * configuration;
};

struct TemplateSourceSelection {
    std::vector<const TemplateBank *> donors;
    std::vector<const TemplateDefinition *> operation_sources;
    std::vector<const TemplateBank *> initial_sources;
    std::optional<TemplateGap> gap;

    nlohmann::json describe() const;
};

// Production owns extraction and binding in one call. The workflow publishes
// the resulting asset and manifest after both stages have completed.
struct TemplateProduction {
    nlohmann::json manifest;
    std::optional<TemplateSource> source;
};

// Native extraction, template production and configuration interpretation end here.
// Selection reuses template rules; the workflow owns costs, mutable controllers
// and combination execution.
class TemplateAdapter {
public:
    virtual ~TemplateAdapter() = default;

    virtual Framework framework() const = 0;
    virtual std::string_view name() const = 0;
    virtual TemplateProduction produce(const std::filesystem::path & manifest_path, nlohmann::json manifest,
                                       const std::filesystem::path & work_directory) const = 0;
    virtual nlohmann::json compare_structure(const std::filesystem::path & target_graph, nlohmann::json facts, const nlohmann::json & environment) const = 0;
    virtual FrameworkConfiguration source_configuration(const nlohmann::json & parameters) const = 0;
    // Shared by source selection and cost candidate eligibility. This describes
    // native configuration semantics, never service-time portability.
    virtual bool shares_execution_path(const FrameworkConfiguration & source, const FrameworkConfiguration & target) const = 0;
    virtual std::expected<ServingLimits, std::string_view> control_limits(const FrameworkConfiguration & configuration) const = 0;
    virtual nlohmann::json configurations(std::span<const Candidate * const> candidates) const = 0;
    virtual TemplateSourceSelection select_sources(const Candidate & candidate, std::span<const TemplateSourceCandidate> sources) const = 0;
};

} // namespace markov
