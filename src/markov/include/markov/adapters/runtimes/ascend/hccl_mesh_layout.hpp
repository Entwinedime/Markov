#pragma once

#include "markov/templates/communication_work.hpp"

#include <expected>
#include <string>

namespace markov {

// Interpret the observed HCOMM mesh implementation within its native branch.
// Structural completion comes from the common task dependency analysis.
class HcclMeshLayout {
public:
    std::expected<CommunicationWorkLayout, std::string> bind(const SourceCollective & source, bool has_serial_completion) const;
};

} // namespace markov
