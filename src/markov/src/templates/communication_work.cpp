#include "markov/templates/communication_work.hpp"
#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>

namespace markov {

std::uint64_t CollectiveCountRule::count(const BatchInput & input) const {
    std::uint64_t extent = input.requests.size();
    if (rows == CollectiveRows::Tokens) {
        extent = 0;
        for (const auto & request : input.requests) extent += request.input_tokens;
    }
    return extent * width;
}

std::expected<HostMessageRule, std::string_view> HostMessageRule::bind(std::uint32_t root, std::span<const std::uint32_t> ranks,
                                                                       const nlohmann::json & dependencies) {
    HostMessageRule rule{ root, {} };
    if (!dependencies.is_array()) return std::unexpected("host_message_completion_dependency_not_qualified");
    for (const auto & edge : dependencies) {
        if (!edge.is_array() || edge.size() != 2) return std::unexpected("host_message_completion_dependency_not_qualified");
        const auto entry = static_cast<std::uint32_t>(input::integer(edge[0], 0, std::numeric_limits<std::uint32_t>::max()));
        const auto done = static_cast<std::uint32_t>(input::integer(edge[1], 0, std::numeric_limits<std::uint32_t>::max()));
        if (std::ranges::find(ranks, entry) == ranks.end() || std::ranges::find(ranks, done) == ranks.end())
            return std::unexpected("host_message_completion_dependency_not_qualified");
        rule.completion_entry_dependencies.emplace_back(entry, done);
    }
    std::ranges::sort(rule.completion_entry_dependencies);
    if (std::ranges::adjacent_find(rule.completion_entry_dependencies) != rule.completion_entry_dependencies.end())
        return std::unexpected("host_message_completion_dependency_not_qualified");
    return rule;
}

} // namespace markov
