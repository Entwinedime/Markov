#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/modules/hicache/host_cpu_service.hpp"
#include "markov/trace_graph/modules/hicache/patch/source_dag_index.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

using namespace markov::trace_graph;
namespace {
core::DagGraph source_graph() {
    std::vector<core::TraceEvent> events;
    for (const auto at : { 100, 110 }) {
        core::TraceEvent event;
        event.source_channel = core::TraceSourceChannel::Torch;
        event.name = "work.self";
        event.cat = "cpu_op";
        event.pid = "1";
        event.tid = "1";
        event.ts = at;
        event.dur = 4;
        events.push_back(event);
    }
    return core::DagBuilder(1).build(std::move(events), 0);
}
nlohmann::json input(double change, uint64_t begin = 100'000, uint64_t end = 110'000) {
    return {
        { "rows", { { { "pid", 1 }, { "tid", 1 }, { "measured_service_delta_us", change }, { "exclusive_ranges_ns", { { begin, end } } } } } }
    };
}
} // namespace
void check_host_cpu_service() {
    for (const bool covered : { false, true }) {
        auto graph = source_graph();
        if (covered) modules::hicache::attach_host_cpu_service(graph, input(2.));
        const auto correction = modules::hicache::attach_recorder_cpu_service(graph,
                                                                              {
                                                                                  { { "pid", 1 },
                                                                                   { "tid", 1 },
                                                                                   { "method", "scope" },
                                                                                   { "method_begin_ns", 101'000 },
                                                                                   { "method_end_ns", 103'000 },
                                                                                   { "thread_cpu_ns", 1'000 } }
        });
        if (correction.at("writes") != (covered ? 0 : 1) || correction.at("added_integer_reduction_us") != (covered ? 0 : 1)
            || graph.cpu_service_cost().duration({ "1", "1" }, 100, 110) != (covered ? 8 : 9))
            throw std::runtime_error("Recorder correction must use only uncovered measured source work");
    }
    {
        core::DagGraph graph;
        const auto enclosing = graph.add_synthetic_node({
            .name = "long",
            .lane_key = "main",
            .duration = 100,
            .observed_point = core::DagObservedPoint{ "1", "1", 100, 0 }
        });
        graph.add_synthetic_node({
            .name = "short",
            .lane_key = "main",
            .duration = 2,
            .observed_point = core::DagObservedPoint{ "1", "1", 120, 0 }
        });
        const auto next = graph.add_synthetic_node({
            .name = "next",
            .lane_key = "main",
            .duration = 3,
            .observed_point = core::DagObservedPoint{ "1", "1", 200, 0 }
        });
        const modules::hicache::patch::HiCacheSourceDagIndex source(graph);
        const auto inside = source.cpu_interval_candidates("1", "1", 150, 160);
        const auto boundary = source.cpu_interval_candidates("1", "1", 200, 201);
        if (inside.empty() || inside.front() != enclosing || boundary.size() != 1 || boundary.front() != next
            || !source.cpu_interval_candidates("missing", "1", 150, 160).empty() || !source.cpu_interval_candidates("1", "1", 300, 301).empty())
            throw std::runtime_error("CPU interval index loses enclosing leaves or includes ended boundary work");
    }

    for (const auto [change, expected] : {
             std::pair{  2.5,  8 },
             {  3.5,  6 },
             { -2.5, 12 },
             { -3.5, 14 },
             {   0., 10 }
    }) {
        auto graph = source_graph();
        modules::hicache::attach_host_cpu_service(graph, input(change));
        if (graph.cpu_service_cost().duration({ "1", "1" }, 100, 110) != expected) throw std::runtime_error("Host CPU signed/tie-rounding budget changed");
    }
    auto graph = source_graph();
    modules::hicache::attach_host_cpu_service(graph, input(2., 101'000, 109'000));
    if (graph.cpu_service_cost().duration({ "1", "1" }, 101, 109) != 6) throw std::runtime_error("Partial self/gap host ownership changed");
    bool rejected = false;
    try {
        modules::hicache::attach_host_cpu_service(graph, input(2., 101'000, 109'000));
    }
    catch (const std::exception &) {
        rejected = true;
    }
    if (!rejected) throw std::runtime_error("Overlapping host service was accepted");

    graph = source_graph();
    for (auto & event : graph.mutable_events())
        if (event.ts == 100) event.name = "semantic_receive";
    auto recorder = input(1., 101'000, 102'000);
    recorder["rows"][0]["method"] = "profiling.recorder_write";
    recorder["rows"][0]["exclusive_ranges_ns"] = {
        { 110'000, 111'000 },
        { 101'000, 102'000 }
    };
    const auto retained = modules::hicache::attach_host_cpu_service(graph, recorder);
    if (retained.size() != 1 || retained[0].at("retained_reduction_us") != 1 || graph.cpu_service_cost().duration({ "1", "1" }, 101, 102) != 1
        || graph.cpu_service_cost().duration({ "1", "1" }, 110, 111) != 1)
        throw std::runtime_error("Ambiguous recorder write must remain wholly uncorrected and reported");
    const auto correction = modules::hicache::attach_recorder_cpu_service(
        graph,
        {
            { { "pid", 1 }, { "tid", 1 }, { "method", "scope" }, { "method_begin_ns", 101'000 }, { "method_end_ns", 102'000 }, { "thread_cpu_ns", 1'000 } }
    });
    if (correction.at("retained_writes") != 1 || correction.at("applied_added_integer_reduction_us") != 0)
        throw std::runtime_error("Single-pass recorder correction must retain ambiguous source boundaries");
    modules::hicache::attach_host_cpu_service(graph, input(1., 110'000, 114'000));
    if (graph.cpu_service_cost().duration({ "1", "1" }, 110, 114) != 3)
        throw std::runtime_error("Retained recorder write must not suppress other valid CPU corrections");
    rejected = false;
    try {
        modules::hicache::attach_host_cpu_service(graph, input(1., 101'000, 102'000));
    }
    catch (const std::exception &) {
        rejected = true;
    }
    if (!rejected) throw std::runtime_error("Ordinary host boundary validation was relaxed");
}
