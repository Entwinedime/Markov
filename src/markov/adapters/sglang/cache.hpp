#pragma once

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

struct CacheSettings {
    std::size_t device_pages, host_pages;
    std::optional<std::size_t> backup_after_hits;
};

CacheSettings read_cache_settings(const nlohmann::json & raw);
trace_graph::planning::CacheBackend cache_backend(const trace_graph::source::SourceContext & source);

} // namespace markov::adapters::sglang
