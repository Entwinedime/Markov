#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "markov/serving/action_graphs.hpp"
#include "markov/serving/control.hpp"
#include "markov/execution/dependencies.hpp"

namespace markov {

class ExecutionInputs;

struct ServingParticipant {
    ActionGraphs & graphs;
    GraphDependencies & dependencies;
};

class ServingSession {
public:
    ServingSession(ServingControl & control, Timeline & timeline, std::map<std::string, ServingParticipant> participants);
    nlohmann::json run(std::int64_t given_work_ns, ExecutionInputs * inputs = nullptr);

private:
    ServingControl & control_;
    Timeline & timeline_;
    std::map<std::string, ServingParticipant> participants_;
};

} // namespace markov
