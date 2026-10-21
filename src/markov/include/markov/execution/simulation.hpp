#pragma once

#include "markov/contracts/control.hpp"
#include "markov/contracts/request.hpp"
#include "markov/execution/execution_graph.hpp"
#include "markov/execution/workspace_state.hpp"
#include "markov/performance/node_costs.hpp"
#include "markov/templates/templates.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
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
    std::optional<nlohmann::json> request_result(RequestId id) const;
    const CostRequirements & requirements() const { return execution_graph_.requirements(); }

private:
    struct QueuedEvent {
        ExecutionEvent event;
        std::uint64_t sequence;
    };

    struct LaterEvent {
        bool operator()(const QueuedEvent & a, const QueuedEvent & b) const;
    };

    struct Work {
        std::size_t pending_ports = 0;
    };

    struct RequestTiming {
        std::optional<std::int64_t> ready_ns;
        std::optional<std::int64_t> first_published_ns;
        std::optional<std::int64_t> completed_ns;
    };

    std::optional<nlohmann::json> accept(std::vector<ControlAction> actions);
    void enqueue(ExecutionEvent event);
    nlohmann::json describe_invocation(const InvokeStep & invocation) const;
    void record_batch(std::span<const InvokeStep> invocations);
    nlohmann::json report(std::string_view status, const nlohmann::json & gaps);

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
    std::size_t work_count_ = 0;
    std::size_t peak_work_ = 0;
    std::size_t batches_ = 0;
    std::uint32_t request_capacity_ = 0;


    std::vector<RequestTiming> request_timings_;
};


} // namespace markov
