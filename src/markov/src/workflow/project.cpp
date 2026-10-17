#include "markov/workflow/project.hpp"

#include <stdexcept>

namespace markov {

const Candidate & Project::configuration(std::string_view id) const {
    for (const auto & candidate : configurations) {
        if (candidate.id == id) return candidate;
    }
    throw std::invalid_argument("unknown configuration: " + std::string(id));
}

const RequestWorkload & Project::workload(std::string_view id) const {
    for (const auto & item : request_workloads) {
        if (item.id == id) return item;
    }
    throw std::invalid_argument("unknown workload: " + std::string(id));
}

} // namespace markov
