#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/contracts/input_group.hpp"
#include "markov/execution/protocol.hpp"

namespace markov {

struct GroupExecutionPlan {
    ExecutionPattern pattern;
    nlohmann::json parts;
};

// An adapter declares the reduction; shared code composes a verified native launch body.
class GroupExecution {
public:
    static GroupExecution compile(const nlohmann::json & declaration, nlohmann::json & pattern, std::span<const Parameters> members);
    static GroupExecution read(const nlohmann::json & source);

    GroupExecutionPlan instantiate(const ExecutionPattern & pattern, const InputGroup & group, const Parameters & parameters,
                                   std::span<const Parameters> members) const;
    const std::string & group() const;
    nlohmann::json document() const;
    void validate(const ExecutionPattern & pattern) const;

private:
    struct Part {
        std::vector<Parameters> inputs;
        Parameters output;
    };

    static GroupExecution declaration(const nlohmann::json & source);
    std::vector<Part> parts(std::span<const Parameters> members) const;

    std::string group_;
    std::string field_;
    std::int64_t fan_in_ = 0;
    std::array<std::size_t, 3> body_{};
};

} // namespace markov
