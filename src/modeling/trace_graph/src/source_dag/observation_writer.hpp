#pragma once

#include <cstddef>
#include <fstream>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>

namespace markov::trace_graph::source_dag {

/** One capture's measurement stream; reports carry only its count. */
class ObservationWriter {
public:
    explicit ObservationWriter(const std::string & path);
    void push_back(const nlohmann::json & observation, std::string_view operation_prefix = {});
    void append(const nlohmann::json & observations, std::string_view operation_prefix = {});
    void finish();
    size_t size() const { return count_; }

private:
    std::ofstream stream_;
    size_t count_ = 0;
};

} // namespace markov::trace_graph::source_dag
