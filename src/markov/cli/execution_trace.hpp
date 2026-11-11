#pragma once

#include <filesystem>
#include <fstream>
#include <map>

#include "markov/execution/observer.hpp"
#include "markov/controller/observer.hpp"
#include "markov/resources/observer.hpp"

namespace markov {

class ExecutionTrace final : public ExecutionObserver, public ResourceObserver, public ControlObserver {
public:
    explicit ExecutionTrace(const std::filesystem::path & path);
    void inserted(const CoarseGraph & graph, std::span<const std::size_t> events, std::span<const ServiceDuration> work_ns, std::int64_t time_ns) override;
    void connected(std::size_t source, std::size_t target, Relation relation, std::int64_t delay_ns) override;
    void completed(std::size_t event, std::int64_t time_ns) override;
    void constrained(std::size_t event, std::int64_t begin_ns, std::int64_t end_ns) override;
    void initialized(const nlohmann::json & instances, const nlohmann::json & requests, std::int64_t clock_epoch_ns) override;
    void input_read(const nlohmann::json & identity, std::span<const std::size_t> requests, bool in_flow, std::int64_t time_ns) override;
    void observed_values(std::string_view group, std::size_t boundary, const nlohmann::json & values, std::int64_t time_ns) override;
    void native_protocol(std::string_view group, const nlohmann::json & record, std::int64_t time_ns) override;
    void queried(std::string_view group, std::size_t boundary, const nlohmann::json & result,
                                    const nlohmann::json & clocks, std::int64_t time_ns) override;
    void completion_read(std::string_view group, std::size_t boundary, std::string_view signal, std::size_t event,
                         bool result, std::int64_t time_ns) override;
    void notification_declared(const nlohmann::json & identity, std::size_t event) override;
    void notification_ready(const nlohmann::json & identity, std::size_t event,
                            std::span<const std::size_t> prerequisites, std::int64_t time_ns) override;
    void applied(std::string_view group, const nlohmann::json & operation, const nlohmann::json & result, const ResourceChanges & changes,
                 std::int64_t time_ns) override;
    void bound(std::size_t invocation, std::string_view pool, std::string_view request, std::optional<std::size_t> slot, std::int64_t begin,
               std::span<const KvPageRange> ranges) override;
    void state_bound(std::size_t invocation, std::string_view pool, std::string_view field, std::string_view request, std::size_t slot) override;
    void finish();

private:
    std::ofstream stream_;
    std::map<std::size_t, std::size_t> completion_reads_;
};

} // namespace markov
