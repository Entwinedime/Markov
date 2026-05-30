#include "trace_graph/export_raw_trace.hpp"
#include "trace_graph/logger.hpp"

#include <fstream>
#include <memory>
#include <vector>

namespace TraceGraph {

void export_raw_trace(const std::string & filename, const std::vector<std::unique_ptr<ActivityRecord>> & records) {
    std::ofstream ofs(filename);
    if (!ofs.is_open()) {
        Logger::instance().error() << "Failed to open " << filename << " for writing raw trace.";
        return;
    }
    ofs << "{\n  \"traceEvents\": [\n";
    bool first = true;
    for (const auto & rec : records) {
        if (rec->ts == 0 || !(rec->dur)) continue;
        if (!first) ofs << ",\n";
        rec->print(ofs);
        first = false;
    }
    ofs << "\n  ]\n}\n";
}

} // namespace TraceGraph
