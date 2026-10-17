#pragma once

#include "markov/contracts/request.hpp"
#include "markov/templates/templates.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string_view>

namespace markov {

// Optional diagnostics own their serialized data. Immutable source descriptions
// appear once; target calls refer to them and retain only instance-specific work.
class ExecutionDetails {
public:
    explicit ExecutionDetails(std::span<const Request> requests);

    static std::string_view connection_rule_name(TemplateConnectionRule rule);

    void add(const InvokeStep & invocation, std::span<const TemplateInstance> instances, const std::map<PortKind, std::int64_t> & completed_ports,
             std::optional<std::int64_t> start_ns);
    void write(const std::filesystem::path & path) const;

private:
    std::size_t source(const TemplateDefinition & definition);

    std::span<const Request> requests_;
    std::map<const TemplateDefinition *, std::size_t> source_indices_;
    nlohmann::json sources_ = nlohmann::json::array();
    nlohmann::json work_ = nlohmann::json::array();
};

} // namespace markov
