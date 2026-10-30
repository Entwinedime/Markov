#pragma once

#include "markov/trace_graph/planning/storage_config.hpp"
#include <cstddef>
#include <optional>

#include <nlohmann/json_fwd.hpp>

namespace markov::trace_graph::source {
class SourceContext;
}

namespace markov::trace_graph::planning {
struct CacheBackend;
}

namespace markov::adapters::sglang {

// Scheduler.init_req_max_new_tokens reserves a page beyond the page-rounded
// prompt and requested outputs. A smaller pool silently truncates that output.
std::size_t minimum_device_tokens(std::size_t prompt_tokens, std::size_t output_tokens, std::size_t page_tokens);

struct CacheSettings {
    std::size_t device_pages, host_pages;
    std::optional<std::size_t> backup_after_hits;
    std::optional<trace_graph::planning::StorageConfig> storage;
};

CacheSettings read_cache_settings(const nlohmann::json & raw);
trace_graph::planning::CacheBackend cache_backend(const trace_graph::source::SourceContext & source);

} // namespace markov::adapters::sglang
