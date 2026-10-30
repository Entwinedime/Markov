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
#include "markov/controller/native_transfer.hpp"
#include "markov/resources/kv_pool.hpp"

namespace markov {

struct NativeParticipant {
    std::string group;
    std::size_t stage;
    KvPool & resources;
};

struct NativeInstance {
    std::string id;
    NativeRole role;
    ServingSettings settings;
    std::size_t batch_limit;
    std::vector<NativeParticipant> participants;
    std::int64_t prefill_chunk_tokens = 0;
    bool mixed_batches = false;
    nlohmann::json expert_routes = nullptr;
};

// The peer owns native queues and progress; this endpoint owns delivery and in-flight work only.
class NativeController final : public ServingControl {
public:
    NativeController(std::vector<Request> requests, std::span<const NativeInstance> instances, ControlPeer & peer, const nlohmann::json & prefixes,
                     std::span<const NativeTransferRoute> transfers = {});

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

    struct Input {
        std::optional<std::size_t> call;
        std::size_t arrived = 0;
        std::optional<std::size_t> sequence;
    };

    std::vector<Request> requests_;
    std::map<std::string, std::unique_ptr<NativeGroup>> groups_;
    std::set<std::string> stopped_;
    std::set<std::string> idle_;
    std::map<std::string, Input> inputs_;
    std::map<std::string, std::string> instances_;
    std::unique_ptr<NativeTransfer> transfer_;
    ControlPeer & peer_;
    std::map<TaskIdentity, std::size_t> running_calls_;
    std::size_t sequence_ = 0;
    bool shutdown_ = false;
};

} // namespace markov
