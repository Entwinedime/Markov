#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "markov/controller/control.hpp"
#include "markov/controller/control_peer.hpp"
#include "markov/resources/kv_pool.hpp"

namespace markov {

// The peer owns native queues and progress; this endpoint owns delivery and in-flight work only.
class NativeController final : public ServingControl {
public:
    NativeController(ServingSettings settings, std::vector<Request> requests, KvPool & resources, std::string group, ControlPeer & peer);

    std::optional<ServingAction> plan(std::int64_t now_ns, const GroupAdmission & admission) override;
    void start(const ServingAction & action) override;
    void finish(const ServingAction & action, std::span<const std::size_t> returned_pages, std::int64_t completed_ns) override;
    std::optional<std::int64_t> next_arrival_ns(std::int64_t now_ns) const override;
    bool done() const override;
    std::size_t free_pages() const override;
    std::size_t request_count() const override;
    const Request & request(std::size_t identity) const override;

private:
    nlohmann::json resource_call(const nlohmann::json & message);
    BatchEffects result_effects(const nlohmann::json & members) const;

    std::vector<Request> requests_;
    KvPool & resources_;
    std::string group_;
    ControlPeer & peer_;
    std::optional<Batch> batch_;
    std::optional<TaskIdentity> running_;
    std::set<std::size_t> released_requests_;
    std::vector<std::size_t> released_pages_;
    std::size_t arrived_ = 0;
    std::size_t completed_ = 0;
    std::size_t sequence_ = 0;
    std::size_t flow_ = 0;
    ServingStep next_ = ServingStep::Stage;
};

} // namespace markov
