#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "markov/serving/action_graphs.hpp"
#include "markov/serving/control.hpp"
#include "markov/execution/dependencies.hpp"
#include "markov/execution/timing.hpp"

namespace markov {

class ExecutionInputs;
class WorkCosts;

struct ServingParticipant {
    ActionGraphs & graphs;
    GraphDependencies & dependencies;
    const WorkCosts * costs;
};

class ServingSession {
public:
    ServingSession(ServingControl & control, Timeline & timeline, std::map<std::string, ServingParticipant> participants);
    nlohmann::json run(ServiceDuration given_work_ns, ExecutionInputs * inputs);

private:
    ServingControl & control_;
    Timeline & timeline_;
    std::map<std::string, ServingParticipant> participants_;
};

} // namespace markov
