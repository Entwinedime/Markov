#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "markov/contracts/topology.hpp"
#include "markov/templates/asset.hpp"

namespace markov::sglang {

// SGLang numbers TP ranks within each stage; core communication uses explicit groups.
class ServingTopology {
public:
    ServingTopology(std::span<const TemplateAsset> assets, std::size_t stages, std::span<const std::int64_t> devices);

    const ParallelTopology & parallel() const;
    const RankGroup & stage(std::size_t index) const;
    std::size_t stage_count() const;
    std::size_t tensor_parallel_size() const;

private:
    static ParallelTopology build(std::span<const TemplateAsset> assets, std::size_t stages, std::span<const std::int64_t> devices);

    std::size_t stages_;
    ParallelTopology parallel_;
};

} // namespace markov::sglang
