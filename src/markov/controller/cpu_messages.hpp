#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/controller/observer.hpp"
#include "markov/controller/execution_inputs.hpp"
#include "markov/execution/timeline.hpp"

namespace markov {

struct CpuActor {
    std::string instance;
    Resource control;
};

// The broker owns numeric payloads and channel positions; this service owns their execution readiness.
class CpuMessages {
public:
    CpuMessages(Timeline & timeline, std::int64_t given_service_ns, ControlObserver * observer, ExecutionInputs * inputs);
    void add_actor(const std::string & group, CpuActor actor);
    void prime();
    void call(std::size_t call, const std::string & group, const nlohmann::json & message);
    void notify(std::size_t event);
    std::vector<std::pair<std::size_t, nlohmann::json>> take_replies();
    std::set<std::size_t> retained_events() const;
    bool shutdown_ready() const;
    nlohmann::json summary() const;

private:
    struct Endpoint {
        std::string group;
        std::size_t actor;
        nlohmann::json descriptor;
        nlohmann::json execution;
        std::optional<std::size_t> entered;
        std::optional<std::size_t> returned;
        std::optional<std::size_t> wait_call;
        std::optional<std::size_t> wait_return;
        nlohmann::json wait_execution;
        std::optional<InputPosition> input;
        bool consumed = false;
    };

    struct Message {
        std::array<std::optional<Endpoint>, 2> endpoints;
        std::optional<std::size_t> wire;
        bool resolved = false;
        bool ready = false;
        EventTiming timing{ 0, {} };
    };

    enum class Phase { Post, Return, Wire, Consume };
    struct Completion {
        Phase phase;
        nlohmann::json key;
        std::size_t endpoint;
        std::optional<std::size_t> call;
    };

    struct Service {
        std::size_t entered;
        std::size_t returned;
        std::optional<InputPosition> input;
    };

    void observe(const nlohmann::json & key, const Endpoint & endpoint, const nlohmann::json & fields);
    void ensure_wire(const nlohmann::json & key, Message & message, const Endpoint & endpoint);
    void resolve(const nlohmann::json & key, Message & message);
    Service service(const nlohmann::json & key, const Endpoint & endpoint,
                    const nlohmann::json & execution, std::optional<std::size_t> prerequisite);
    void consume(const nlohmann::json & key, Endpoint & endpoint);
    void post(std::size_t call, const std::string & group, const nlohmann::json & message);
    void wait(std::size_t call, const std::string & group, const nlohmann::json & message);

    Timeline & timeline_;
    std::int64_t given_service_ns_;
    ControlObserver * observer_;
    ExecutionInputs * inputs_;
    std::map<std::string, CpuActor> actors_;
    std::map<nlohmann::json, Message> messages_;
    std::map<std::size_t, std::vector<Completion>> completions_;
    std::vector<std::pair<std::size_t, nlohmann::json>> replies_;
    bool priming_ = true;
    std::size_t service_count_ = 0;
    std::size_t ready_count_ = 0;
    std::size_t consumed_count_ = 0;
};

} // namespace markov
