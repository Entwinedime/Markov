#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
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

struct NativeParticipant {
    std::string group;
    std::size_t stage;
    KvPool & resources;
};

// The peer owns native queues and progress; this endpoint owns delivery and in-flight work only.
class NativeController final : public ServingControl {
public:
    NativeController(ServingSettings settings, std::vector<Request> requests, std::span<const NativeParticipant> participants, std::size_t batch_limit,
                     ControlPeer & peer);

    std::optional<ServingAction> plan(std::int64_t now_ns, const GroupAdmission & admission) override;
    void start(const ServingAction & action) override;
    void finish(const ServingAction & action, const BatchProgram & program, std::int64_t completed_ns) override;
    std::optional<std::int64_t> next_arrival_ns(std::int64_t now_ns) const override;
    bool done() const override;
    std::size_t free_pages() const override;
    std::size_t request_count() const override;
    const Request & request(std::size_t identity) const override;

private:
    nlohmann::json receive_call();
    void reply(std::size_t call, const nlohmann::json & result);

    std::vector<Request> requests_;
    std::map<std::string, std::unique_ptr<NativeGroup>> groups_;
    std::set<std::string> stopped_;
    std::set<std::string> idle_;
    std::string input_group_;
    ControlPeer & peer_;
    std::map<TaskIdentity, std::size_t> running_calls_;
    std::optional<std::size_t> input_call_;
    std::size_t arrived_ = 0;
    std::optional<std::size_t> idle_sequence_;
    std::size_t input_sequence_ = 0;
    std::size_t sequence_ = 0;
    bool shutdown_ = false;
};

} // namespace markov
