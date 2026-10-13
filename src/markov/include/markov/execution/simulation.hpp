#pragma once

#include "markov/contracts/control.hpp"
#include "markov/contracts/project.hpp"
#include "markov/execution/execution_graph.hpp"
#include "markov/execution/workspace_state.hpp"
#include "markov/performance/node_costs.hpp"
#include "markov/templates/templates.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <queue>
#include <span>
#include <string_view>
#include <vector>

namespace markov {

class SimulationRuntime {
public:
    SimulationRuntime(std::span<const Request> requests, Controller & controller, const TemplateBank & templates, const NodeCosts & costs,
                      std::optional<std::filesystem::path> graph_path);

    nlohmann::json run(ServingLimits limits);

private:
    struct QueuedEvent {
        ControlEvent event;
        std::uint64_t sequence;
    };

    struct LaterEvent {
        bool operator()(const QueuedEvent & a, const QueuedEvent & b) const;
    };

    struct Work {
        InvokeStep invocation;
        std::vector<TemplateInstance> instances;
        std::optional<std::int64_t> start_ns;
        std::map<PortKind, std::int64_t> completed_ports;
    };

    struct RequestTiming {
        std::optional<std::int64_t> first_published_ns;
        std::optional<std::int64_t> completed_ns;
    };

    std::optional<nlohmann::json> accept(std::span<const ControlAction> actions);
    void enqueue(ControlEvent event);
    nlohmann::json report(std::string_view status, const nlohmann::json & gaps) const;

    std::span<const Request> requests_; // Borrowed from the selected workload for this simulation.
    Controller & controller_;
    const TemplateBank & templates_;
    const NodeCosts & costs_;
    TemplateAssembly assembly_;
    WorkspaceState workspace_;
    ExecutionGraph execution_graph_;
    std::optional<std::filesystem::path> graph_path_;

    // The event queue owns simulated time; sequence breaks ties without advancing it.
    std::int64_t time_ns_ = 0;
    std::uint64_t next_event_sequence_ = 0;
    std::priority_queue<QueuedEvent, std::vector<QueuedEvent>, LaterEvent> events_;

    std::map<WorkId, Work> work_;
    std::size_t batches_ = 0;
    std::uint32_t request_capacity_ = 0;


    std::vector<RequestTiming> request_timings_;
};


} // namespace markov
