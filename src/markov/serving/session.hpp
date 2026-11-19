#pragma once

#include <map>
#include <string>

#include "markov/serving/action_graphs.hpp"
#include "markov/serving/control.hpp"
#include "markov/execution/dependencies.hpp"

namespace markov {

class ServiceTimes;

struct ServingParticipant {
    ActionGraphs & graphs;
    GraphDependencies & dependencies;
};

class ServingSession {
public:
    ServingSession(ServingControl & control, Timeline & timeline, ServiceTimes & times, std::map<std::string, ServingParticipant> participants);
    nlohmann::json run();

private:
    ServingControl & control_;
    Timeline & timeline_;
    ServiceTimes & times_;
    std::map<std::string, ServingParticipant> participants_;
};

} // namespace markov
