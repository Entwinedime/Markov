#pragma once

#include "markov/production/block_binding.hpp"

#include <nlohmann/json_fwd.hpp>

#include <span>

namespace markov {

// Module identities are specific to SGLang's Qwen3 implementation, not the core model contract.
class Qwen3Regions {
public:
    static bool supports(const nlohmann::json & configuration);
    static std::span<const NativeRegionBinding> bindings();
};

} // namespace markov
