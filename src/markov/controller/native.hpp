#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/contracts/serving.hpp"
#include "markov/controller/action_graphs.hpp"
#include "markov/controller/execution_inputs.hpp"
#include "markov/controller/control.hpp"
#include "markov/controller/control_peer.hpp"
#include "markov/controller/cpu_messages.hpp"
#include "markov/controller/delivery.hpp"
#include "markov/controller/observer.hpp"
#include "markov/execution/dependencies.hpp"
#include "markov/resources/kv_pool.hpp"
#include "markov/resources/observer.hpp"
#include "markov/resources/service.hpp"

namespace markov {

struct NativeParticipant {
    std::string group;
    std::size_t rank;
    Resource control;
    ResourceService & resources;
    ActionGraphs & graphs;
    GraphDependencies & dependencies;
};

struct NativeInstance {
    std::string id;
    ServingSettings settings;
    std::optional<std::int64_t> microbatch_requests;
    std::vector<NativeParticipant> participants;
    std::int64_t prefill_chunk_tokens = 0;
    bool mixed_batches = false;
    nlohmann::json expert_routes = nullptr;
};

// The peer owns native queues and progress; this endpoint owns delivery and in-flight work only.
class NativeController final : public ServingControl {
public:
    NativeController(std::vector<Request> requests, std::span<const NativeInstance> instances, ControlPeer & peer, const nlohmann::json & prefixes,
                     Timeline & timeline, const nlohmann::json & ingress, std::int64_t given_work_ns,
                     ExecutionInputs * execution_inputs = nullptr, ResourceObserver * observer = nullptr,
                     ControlObserver * control_observer = nullptr);

    std::optional<ServingAction> plan(std::int64_t now_ns) override;
    void finish(const ServingAction & action, std::size_t returned_event, std::int64_t completed_ns,
                const std::map<std::string, StorageBinding> & values) override;
    bool notify(std::size_t event) override;
    std::set<std::size_t> retained_events() const override;
    std::optional<std::int64_t> next_wakeup_ns(std::int64_t now_ns) const override;
    bool done() const override;
    std::size_t free_pages() const override;
    std::size_t request_count() const override;
    const Request & request(std::size_t identity) const override;
    nlohmann::json cpu_summary() const;

private:
    nlohmann::json receive_call();
    void reply(std::size_t call, const nlohmann::json & result);
    void reach_idle(const std::string & group, std::size_t call, bool waiting);
    bool release_idle(const std::string & instance);
    std::optional<std::size_t> completion_event(const std::string & signal) const;
    std::size_t read_boundary(const std::string & group, const nlohmann::json & message) const;
    void resolve_notifications();
    bool cpu_call(std::size_t call, const std::string & group, const nlohmann::json & message);
    bool release_cpu();

    struct CompletionNotice {
        nlohmann::json identity;
        std::size_t request;
        std::optional<std::size_t> call;
        std::vector<std::string> signals;
        std::vector<std::size_t> prerequisites;
    };
    std::size_t notification_event(const nlohmann::json & identity, std::size_t request);
    void release_notifications(std::size_t request);

    struct Input {
        RequestDelivery delivery;
        std::optional<std::size_t> call = std::nullopt;
        nlohmann::json identity = nullptr;
        std::optional<std::int64_t> return_ns = std::nullopt;
        bool waiting = false;
        bool woken = false;
        bool in_flow = false;
    };

    struct PendingRead {
        std::size_t boundary;
        std::int64_t time_ns;
    };

    nlohmann::json resource_call(const std::string & group, const nlohmann::json & message);

    struct Participant {
        ResourceService & resources;
        ActionGraphs & graphs;
        GraphDependencies & dependencies;
    };

    std::vector<Request> requests_;
    std::map<std::string, Participant> groups_;
    std::map<std::string, std::set<std::size_t>> completed_;
    std::set<std::string> stopped_;
    std::map<std::string, std::optional<std::size_t>> idle_;
    std::map<std::string, Input> inputs_;
    std::map<std::string, std::string> instances_;
    ControlPeer & peer_;
    Timeline & timeline_;
    ExecutionInputs * execution_inputs_;
    ResourceObserver * observer_;
    ControlObserver * control_observer_;
    CpuMessages cpu_;
    std::map<std::size_t, std::set<std::string>> observed_;
    std::map<nlohmann::json, std::size_t> notification_ids_;
    std::map<std::size_t, CompletionNotice> notices_;
    std::set<std::size_t> pending_notices_;
    std::map<TaskIdentity, std::size_t> running_calls_;
    std::map<std::string, PendingRead> pending_reads_;
    std::size_t next_action_ = 0;
    bool shutdown_ = false;
};

} // namespace markov
