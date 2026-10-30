#pragma once

#include "markov/trace_graph/frontend/model_config.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
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
    size_t size() const { return packages_.size(); }

private:
    std::map<std::string, CostPackage> packages_;
};

} // namespace markov::trace_graph::frontend
