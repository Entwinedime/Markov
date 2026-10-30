#pragma once

#include "markov/trace_graph/frontend/model_config.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace markov::trace_graph::frontend {

struct CostPackage {
    uint64_t kv_bytes_per_token;
    nlohmann::json io;
    std::shared_ptr<const cost::OperatorModels> operators;

    HiCacheIoCostConfig io_for_page(uint64_t page_bytes) const;
};

/** Session-owned inputs, loaded before forking targets and never changed by them. */
class CostPackages {
public:
    const CostPackage & get(const std::string & filename);
    size_t size() const { return package_.has_value() ? 1 : 0; }

private:
    std::string path_;
    std::optional<CostPackage> package_;
};

} // namespace markov::trace_graph::frontend
