#pragma once

#include "trace_graph/activity_record.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace TraceGraph {

class TraceParser {
public:
    void parse(const std::string & filename, std::vector<std::unique_ptr<ActivityRecord>> & records);

private:
    const char * p = nullptr;
    const char * end = nullptr;

    void skip_ws();
    std::string_view parse_string();
    std::string_view parse_primitive();
    void skip_value();
};

void parse_trace_json(const std::string & filename, std::vector<std::unique_ptr<ActivityRecord>> & records);

} // namespace TraceGraph
