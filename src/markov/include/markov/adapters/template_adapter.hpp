#pragma once

#include "markov/contracts/control.hpp"
#include "markov/contracts/project.hpp"
#include "markov/templates/templates.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
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
    std::uint32_t ranks;
    std::vector<const TemplateBank *> donors;
    std::vector<const TemplateDefinition *> operation_sources;
    std::vector<const TemplateBank *> initial_sources;
    std::optional<TemplateGap> gap;
};

// Native extraction, template production and configuration interpretation end here.
// Selection reuses template rules; the workflow owns costs, mutable controllers
// and combination execution.
class TemplateAdapter {
public:
    virtual ~TemplateAdapter() = default;

    virtual Framework framework() const = 0;
    virtual std::string_view name() const = 0;
    virtual nlohmann::json extract(const std::filesystem::path & manifest_path, nlohmann::json manifest) const = 0;
    virtual TemplateSource produce(const std::filesystem::path & manifest_path) const = 0;
    virtual FrameworkConfiguration source_configuration(const nlohmann::json & parameters) const = 0;
    // Candidate eligibility only. The cost model must still match target work;
    // equal implementation paths do not prove service-time portability.
    virtual bool supports_cost_configuration(const FrameworkConfiguration & source, const FrameworkConfiguration & target) const = 0;
    virtual std::expected<ServingLimits, std::string_view> control_limits(const FrameworkConfiguration & configuration) const = 0;
    virtual nlohmann::json configurations(std::span<const Candidate * const> candidates) const = 0;
    virtual TemplateSourceSelection select_sources(const Candidate & candidate, std::span<const TemplateSourceCandidate> sources) const = 0;
    virtual nlohmann::json describe_sources(const TemplateSourceSelection & selection) const = 0;
};

} // namespace markov
