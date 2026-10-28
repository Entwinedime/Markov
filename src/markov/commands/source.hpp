#pragma once

#include <cstddef>
#include <span>

#include <nlohmann/json.hpp>

namespace markov::adapters {
struct PreparedSource;
struct SelectedCosts;
} // namespace markov::adapters

namespace markov::adapters::sglang {
struct CacheSettings;
}

namespace markov::commands {

nlohmann::json source_report(const adapters::PreparedSource & source);
nlohmann::json page_work_report(const adapters::PreparedSource & source, std::span<const std::size_t> page_sizes_tokens,
                                const adapters::SelectedCosts * costs = nullptr, const adapters::sglang::CacheSettings * cache = nullptr);

} // namespace markov::commands
