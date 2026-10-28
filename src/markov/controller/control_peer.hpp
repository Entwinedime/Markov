#pragma once

#include <nlohmann/json.hpp>

namespace markov {

// Workflow owns transport; the control adapter exchanges only resource and work values.
class ControlPeer {
public:
    virtual ~ControlPeer() = default;
    virtual nlohmann::json receive() = 0;
    virtual void send(const nlohmann::json & message) = 0;
};

} // namespace markov
