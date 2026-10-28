#pragma once

#include <cstddef>
#include <span>

#include <nlohmann/json.hpp>

namespace markov::adapters {
struct PreparedSource;
struct SelectedCosts;
} // namespace markov::adapters

namespace markov::commands {

nlohmann::json source_report(const adapters::PreparedSource & source);
nlohmann::json page_work_report(const adapters::PreparedSource & source, std::span<const std::size_t> page_sizes_tokens,
                                const adapters::SelectedCosts * costs = nullptr);

} // namespace markov::commands
