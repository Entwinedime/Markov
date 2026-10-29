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
#include "markov/resources/kv_pool.hpp"

namespace markov {

enum class NativeRole { Model, Prefill, Decode };

// One native participant group owns its work; the borrowed pool is its only resource ledger.
class NativeGroup {
public:
    NativeGroup(const std::vector<Request> & requests, KvPool & resources, std::string group, std::size_t stage, NativeRole role);
    NativeGroup(const NativeGroup &) = delete;
    NativeGroup & operator=(const NativeGroup &) = delete;

    void begin_iteration(std::size_t flow, std::size_t lane);
    std::optional<ServingAction> plan(const nlohmann::json & message, std::int64_t now_ns, std::size_t delivered);
    nlohmann::json resource_call(const nlohmann::json & message, std::size_t delivered);
    void start(const ServingAction & action);
    void finish(const ServingAction & action, std::span<const std::size_t> returned_pages);
    bool caught_up(std::size_t delivered) const;
    bool done() const;
    bool settled() const;
    std::size_t free_pages() const;

private:
    Batch resident_batch(const nlohmann::json & requests, std::size_t delivered) const;
    struct Frame {
        std::shared_ptr<const BatchWork> work;
        std::size_t lane;
        ServingStep next = ServingStep::Prepare;
        std::set<std::size_t> released_requests;
        std::vector<std::size_t> released_pages;
        std::set<std::string> messages;
        bool released = false;
    };
    BatchEffects result_effects(const Frame & frame, const nlohmann::json & members) const;

    // Requests and pool outlive the participant; completed actions never retain another resource account.
    const std::vector<Request> & requests_;
    KvPool & resources_;
    std::string group_;
    std::size_t stage_;
    NativeRole role_;
    std::map<std::size_t, Frame> frames_;
    std::map<std::size_t, std::shared_ptr<const BatchWork>> reservations_;
    std::map<std::size_t, std::size_t> lanes_;
    std::optional<TaskIdentity> running_;
    std::vector<std::size_t> released_pages_;
    std::size_t completed_ = 0;
    std::size_t sequence_ = 0;
};

} // namespace markov
