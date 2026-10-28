#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/contracts/serving.hpp"
#include "markov/resources/kv_pool.hpp"

namespace markov {

// One native participant group owns its work; the borrowed pool is its only resource ledger.
class NativeGroup {
public:
    NativeGroup(const std::vector<Request> & requests, KvPool & resources, std::string group);
    NativeGroup(const NativeGroup &) = delete;
    NativeGroup & operator=(const NativeGroup &) = delete;

    void begin_iteration();
    void end_iteration();
    std::optional<ServingAction> plan(const nlohmann::json & message, std::int64_t now_ns, std::size_t delivered);
    nlohmann::json resource_call(const nlohmann::json & message, std::size_t delivered);
    void start(const ServingAction & action);
    void finish(const ServingAction & action, std::span<const std::size_t> returned_pages);
    bool done() const;
    std::size_t free_pages() const;
    std::size_t sequence() const;

private:
    Batch resident_batch(const nlohmann::json & requests, std::size_t delivered) const;
    BatchEffects result_effects(const nlohmann::json & members) const;

    // Requests and pool outlive the participant; completed actions never retain another resource account.
    const std::vector<Request> & requests_;
    KvPool & resources_;
    std::string group_;
    std::optional<Batch> batch_;
    std::optional<TaskIdentity> running_;
    std::set<std::size_t> released_requests_;
    std::vector<std::size_t> released_pages_;
    std::size_t completed_ = 0;
    std::size_t sequence_ = 0;
    std::size_t flow_ = 0;
    ServingStep next_ = ServingStep::Batch;
};

} // namespace markov
