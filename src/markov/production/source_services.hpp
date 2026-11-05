#pragma once

#include <nlohmann/json.hpp>

namespace markov {

// A dynamic caller owns closed private CPU work; its public child calls retain their own boundaries.
class SourceServices {
public:
    explicit SourceServices(nlohmann::json source);
    nlohmann::json document();

private:
    nlohmann::json source_;
};

} // namespace markov
