#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/contracts/serving.hpp"
#include "markov/controller/control.hpp"
#include "markov/controller/control_peer.hpp"
#include "markov/controller/native_group.hpp"
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
    nlohmann::json receive_call();
    void reply(std::size_t call, const nlohmann::json & result);

    std::vector<Request> requests_;
    NativeGroup group_;
    ControlPeer & peer_;
    std::optional<std::size_t> running_call_;
    std::optional<std::size_t> input_call_;
    std::size_t arrived_ = 0;
    std::optional<std::size_t> input_sequence_;
};

} // namespace markov
