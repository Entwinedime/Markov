#pragma once

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

// Native calls submit transport intent; graph completion alone makes the payload readable.
class NativeTransfer {
public:
    explicit NativeTransfer(std::span<const NativeTransferRoute> routes);

    nlohmann::json call(const nlohmann::json & message);
    void submit(const ServingAction & action, const nlohmann::json & payload);
    std::optional<ServingAction> plan(std::int64_t now_ns);
    void finish(const ServingAction & action);
    bool running() const;
    bool settled() const;

private:
    struct Fragment {
        BatchMember member;
        bool final;
        bool ready = false;
        std::set<TransferBinding> submitted;
        std::set<TransferBinding> completed;
    };

    struct Endpoint {
        bool registered = false;
        bool ready = false;
        bool consumed = false;
        nlohmann::json payload;
        std::int64_t submitted_tokens = 0;
        std::map<std::size_t, Fragment> fragments;
    };

    struct Handoff {
        std::map<std::string, Endpoint> endpoints;
        std::set<TransferBinding> completed;
    };

    std::vector<NativeTransferRoute> routes_;
    std::map<std::size_t, Handoff> handoffs_;
    std::size_t sequence_ = 0;
};

} // namespace markov
