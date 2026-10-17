#pragma once

#include "markov/contracts/service_work.hpp"
#include "markov/contracts/source_observations.hpp"
#include "markov/graph/source_graph.hpp"
#include "markov/production/source_call.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace markov {

class SourceBindings;

// Consumes only files explicitly referenced by a role=base manifest. This is
// source extraction, not prediction or performance fitting. Incomplete causal
// observations remain gaps; a parsed graph never implies costs are available.
class AscendSourceExtractor {
public:
    explicit AscendSourceExtractor(const SourceBindings & bindings) : bindings_(bindings) {}

    nlohmann::json extract(const std::filesystem::path & manifest_path, nlohmann::json manifest, const std::filesystem::path & work_directory);
    static std::optional<DeviceCommandWork> device_command(const nlohmann::json & interval);

private:
    struct Notification {
        SourceSpanId span;
        std::size_t detail;
    };

    bool device_precedes(SourceSpanId before, SourceSpanId after, const std::map<BoundaryId, std::vector<BoundaryId>> & task_predecessors) const;
    bool order_notifications(std::vector<Notification> & notifications, const std::map<BoundaryId, std::vector<BoundaryId>> & task_predecessors) const;

    void read_calls(const nlohmann::json & records);
    void read_intervals(const nlohmann::json & intervals);
    bool observer_work(SourceSpanId interval) const;
    void read_cpu_scopes();
    void read_operations(const nlohmann::json & intervals, const nlohmann::json & flows);
    SourceSpanId operation_host(SourceSpanId call) const;
    void read_flows(const nlohmann::json & flows);
    void read_device_order(const nlohmann::json & intervals, const nlohmann::json & records);
    bool read_task_dependencies(const std::vector<SourceSpanId> & spans, const nlohmann::json & intervals, const nlohmann::json & record,
                                std::size_t source_api);

    void extract_cpu_control();
    void extract_worker_cpu();
    void observe_cpu_control(std::size_t lane, std::int64_t begin_ns, std::int64_t end_ns, std::size_t owner, std::optional<std::size_t> source_interval,
                             std::optional<std::size_t> recorded_scope = std::nullopt);

    std::size_t read_transfers(const nlohmann::json & intervals, const nlohmann::json & records);
    void read_communication(const nlohmann::json & intervals);
    void read_event_signals(const nlohmann::json & records, const nlohmann::json & intervals);
    std::expected<std::vector<SourceSpanId>, std::string> event_wait_frontier(const nlohmann::json & record, const nlohmann::json & intervals) const;
    void read_copy_boundaries(const nlohmann::json & records, const nlohmann::json & intervals);
    void read_host_waits(const nlohmann::json & records, const nlohmann::json & intervals);

    void complete_device_work();

    const SourceBindings & bindings_; // Owned by the production entry for the complete extraction.
    SourceGraph graph_;
    std::vector<SourceCall> calls_;
    std::unordered_map<std::string, std::size_t> call_ids_;

    std::vector<std::vector<std::size_t>> native_api_members_;
    std::vector<SourceSpanId> host_waits_;
    // Chain/position exists only where read_device_order established a path.
    std::map<SourceSpanId, std::pair<std::size_t, std::size_t>> device_order_;
    std::map<std::size_t, std::set<std::int64_t>> cpu_service_boundaries_;
    std::map<std::size_t, std::map<std::int64_t, std::optional<std::size_t>>> cpu_scope_changes_;
    std::map<std::pair<std::size_t, std::size_t>, std::vector<SourceSpanId>> cpu_operation_roots_;
    // CPU lanes already include rank. Windows are merged in profiler time.
    std::map<std::size_t, std::vector<std::pair<std::int64_t, std::int64_t>>> observer_windows_;

    nlohmann::json gaps_ = nlohmann::json::array();
    bool native_dense_path_ = false;
};

} // namespace markov
