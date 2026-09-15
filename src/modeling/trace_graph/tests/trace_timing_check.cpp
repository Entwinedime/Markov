/** @file Small trace timing checks, enabled only in explicit validation builds. */
#include "markov/trace_graph/core/dag_builder.hpp"
#include "../src/io/trace_channel_join.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace markov::trace_graph;

namespace {
core::TraceEvent event(std::string name, std::string pid, std::string tid, uint64_t ts, uint64_t dur,
                       std::string category = "cpu_op") {
    core::TraceEvent result;
    result.source_channel = core::TraceSourceChannel::Torch;
    result.name = std::move(name); result.pid = std::move(pid); result.tid = std::move(tid);
    result.ts = ts; result.dur = dur; result.cat = std::move(category);
    return result;
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void cann_display_process_is_not_a_second_cpu_thread() {
    auto metadata = event("process_name", "900", "0", 0, 0, "");
    metadata.ph = 'M'; metadata.set_arg("name", "CANN");
    auto scalar = event("aten::_local_scalar_dense", "20", "21", 100, 500);
    auto sync = event("AscendCL@aclrtSynchronizeStreamWithTimeout", "900", "21", 110, 480, "");
    auto kernel = event("kernel", "900", "21", 120, 400, "Kernel");
    kernel.set_arg("Physic Stream Id", "3");
    std::vector<core::TraceEvent> events{metadata, scalar, sync, kernel};
    io::detail::retain_duration_events(events);
    require(events[1].pid == "20", "CANN CPU event must share the framework thread's process");
    require(events[1].arg("profiler_display_pid") == "900", "original display identity remains available");
    require(events[2].pid == "900", "device event identity must not be rewritten");
    auto graph = core::DagBuilder(1).build(std::move(events), 0);
    require(std::ranges::none_of(graph.events(), [](const auto& e) {return e.name == "aten::_local_scalar_dense";}),
            "outer blocking call must not survive as another full-duration CPU leaf");
    require(std::ranges::any_of(graph.events(), [](const auto& e) {return e.name.starts_with("AscendCL@aclrtSynchronize");}),
            "nested synchronization must remain a graph event");

    std::vector<core::TraceEvent> no_metadata{scalar, sync};
    io::detail::retain_duration_events(no_metadata);
    require(no_metadata[1].pid == "900", "missing CANN metadata cannot justify merging processes");
    std::vector<core::TraceEvent> ambiguous{scalar, sync, event("other", "30", "21", 0, 10), metadata};
    io::detail::retain_duration_events(ambiguous);
    require(ambiguous[1].pid == "900", "ambiguous thread ownership cannot merge processes");
    std::vector<core::TraceEvent> unknown{metadata, event("runtime", "900", "unknown", 0, 10, "")};
    io::detail::retain_duration_events(unknown);
    require(unknown[0].pid == "900", "unknown runtime threads must retain their identity");
}
}

int main() {
    cann_display_process_is_not_a_second_cpu_thread();
    std::cout << "Trace timing checks passed\n";
}
