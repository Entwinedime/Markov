#pragma once

#include <istream>
#include <ostream>

#include "markov/controller/control_peer.hpp"

namespace markov {

class ControlStream final : public ControlPeer {
public:
    ControlStream(std::istream & input, std::ostream & output);
    nlohmann::json receive() override;
    void send(const nlohmann::json & message) override;

private:
    std::istream & input_;
    std::ostream & output_;
};

} // namespace markov
