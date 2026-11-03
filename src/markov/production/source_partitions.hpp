#pragma once

#include <nlohmann/json.hpp>

namespace markov {

// A declared resource call divides its caller into independently bindable work.
nlohmann::json partition_source(nlohmann::json source);

} // namespace markov
