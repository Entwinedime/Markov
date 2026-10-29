#pragma once

#include <map>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "markov/contracts/serving.hpp"
#include "markov/resources/kv_pool.hpp"

namespace markov {

// Native calls submit transport intent; graph completion alone makes the payload readable.
class NativeTransfer {
public:
    NativeTransfer(std::string source, std::string destination, KvPool & source_pool, KvPool & destination_pool);

    nlohmann::json call(const nlohmann::json & message);
    std::optional<ServingAction> plan(std::int64_t now_ns);
    void finish(const ServingAction & action);
    bool settled() const;

private:
    struct Handoff {
        bool sender = false;
        bool receiver = false;
        bool reserved = false;
        bool prepared = false;
        bool submitted = false;
        bool completed = false;
        bool received = false;
        bool released = false;
        nlohmann::json payload;
    };

    std::string source_;
    std::string destination_;
    KvPool & source_pool_;
    KvPool & destination_pool_;
    std::map<std::size_t, Handoff> handoffs_;
    std::size_t sequence_ = 0;
};

} // namespace markov
