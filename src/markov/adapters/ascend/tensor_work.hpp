#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>

namespace markov::adapters::ascend {

// Independent runtime measurements share the source DAG's device ownership rules.
nlohmann::json tensor_work_report(const std::filesystem::path & capture);

} // namespace markov::adapters::ascend
