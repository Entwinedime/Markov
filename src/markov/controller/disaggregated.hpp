#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "markov/controller/batch_replay.hpp"
#include "markov/controller/control.hpp"
#include "markov/resources/kv_pool.hpp"

namespace markov {

struct DisaggregatedGroups {
    std::string prefill;
    std::string decode;
    std::string transfer;
};

// Request progress is unique; each instance independently owns its slots and KV residence.
// Requests are validated in arrival order; the caller keeps both resource pools and replay alive.
class DisaggregatedController final : public ServingControl {
public:
    DisaggregatedController(ServingSettings prefill, ServingSettings decode, std::vector<Request> requests, DisaggregatedGroups groups,
                            KvPool & prefill_resources, KvPool & decode_resources, BatchReplay * replay = nullptr);

    std::optional<ServingAction> plan(std::int64_t now_ns, const GroupAdmission & admission) override;
    void start(const ServingAction & action) override;
    void finish(const ServingAction & action, const BatchProgram & program, std::int64_t completed_ns) override;
    std::optional<std::int64_t> next_arrival_ns(std::int64_t now_ns) const override;
    bool done() const override;
    std::size_t free_pages() const override;
    std::size_t request_count() const override;
    const Request & request(std::size_t identity) const override;
    std::map<std::string, std::size_t> pool_free_pages() const;

private:
    enum class RequestStage { Waiting, Reserved, ModelReady, Transferred, Active, Finished };

    struct RequestState {
        Request input;
        RequestStage stage = RequestStage::Waiting;
        std::int64_t generated = 0;
        std::size_t flow = 0;

        bool transferred() const;
    };

    Batch view(std::size_t request, const KvPool & pool) const;
    ServingAction action(ServingStep step, Batch batch, BatchEffects effects, std::int64_t now_ns, std::size_t flow) const;
    BatchEffects effects(const Batch & batch, bool first_output) const;
    std::optional<std::size_t> next_waiting() const;
    std::optional<TaskIdentity> & channel(ServingStep step);

    ServingSettings prefill_settings_;
    ServingSettings decode_settings_;
    DisaggregatedGroups groups_;
    std::vector<RequestState> requests_;
    KvPool & prefill_;
    KvPool & decode_;
    std::vector<std::size_t> prefill_active_;
    std::vector<std::size_t> active_;
    std::size_t waiting_ = 0;
    std::size_t next_task_ = 0;
    std::int64_t last_event_ns_ = 0;
    std::optional<TaskIdentity> prefill_busy_;
    std::optional<TaskIdentity> decode_busy_;
    std::optional<TaskIdentity> transfer_busy_;
    BatchReplay * replay_;
};

} // namespace markov
