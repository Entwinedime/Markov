#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "markov/contracts/topology.hpp"
#include "markov/templates/asset.hpp"

namespace markov::sglang {

// SGLang numbers TP ranks within each stage; core communication uses explicit groups.
class ServingTopology {
public:
    ServingTopology(std::span<const TemplateAsset> assets, std::span<const std::int64_t> devices);

    const ParallelTopology & parallel() const;
    const RankGroup & instance(const std::string & name) const;
    const RankGroup & stage(const std::string & name, std::size_t index) const;
    std::size_t stage_count(const std::string & name) const;
    std::size_t tensor_parallel_size(const std::string & name) const;

private:
    static ParallelTopology build(std::span<const TemplateAsset> assets, std::span<const std::int64_t> devices);

    ParallelTopology parallel_;
};

} // namespace markov::sglang
