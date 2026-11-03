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
#include "markov/resources/kv_pool.hpp"

namespace markov {

struct NativeTransferRoute {
    TransferBinding binding;
    KvPool & source;
    KvPool & destination;
};

// Copies, source publication and destination notification advance at their own graph ports.
class NativeTransfer {
public:
    explicit NativeTransfer(std::span<const NativeTransferRoute> routes);

    nlohmann::json poll(const nlohmann::json & message) const;
    void submit(const ServingAction & action, const nlohmann::json & payload);
    std::optional<ServingAction> plan(std::int64_t now_ns);
    void start(const ServingAction & action);
    void finish(const ServingAction & action);
    bool running() const;
    bool settled() const;

private:
    enum class Progress { Waiting, Submitted, Complete };

    struct Fragment {
        BatchMember member;
        bool final;
        bool ready = false;
        std::set<TransferBinding> submitted;
        std::set<TransferBinding> completed;
    };

    struct Source {
        Progress connected = Progress::Waiting;
        Progress published = Progress::Waiting;
        nlohmann::json payload;
        std::int64_t submitted_tokens = 0;
        std::map<std::size_t, Fragment> fragments;
        bool released = false;
    };

    struct Destination {
        Progress published = Progress::Waiting;
        bool consumed = false;
    };

    struct Handoff {
        std::map<std::string, Source> sources;
        std::map<std::string, Destination> destinations;
        std::set<TransferBinding> copied;
    };

    ServingAction endpoint_event(std::size_t request, const std::string & group, ServingStep step, std::int64_t now_ns);

    std::vector<NativeTransferRoute> routes_;
    std::map<std::size_t, Handoff> handoffs_;
    std::size_t sequence_ = 0;
};

} // namespace markov
