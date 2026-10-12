#pragma once

#include "markov/production/block_extractor.hpp"

namespace markov {

class SGLangBlockExtractor final : public BlockExtractor {
public:
    using BlockExtractor::BlockExtractor;

private:
    nlohmann::json resolve_ports(std::size_t call_index) const override;
};

} // namespace markov
