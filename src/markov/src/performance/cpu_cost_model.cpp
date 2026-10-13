#include "markov/performance/cpu_cost_model.hpp"

#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>

#include <limits>
#include <stdexcept>

namespace markov {

CpuCostModel::CpuCostModel(CpuCostUnit unit, const nlohmann::json & entries) : unit_(unit) {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    for (const auto & entry : entries) {
        const auto duration = input::integer(entry.at("estimate_ns"), 0, maximum);
        if (unit_ == CpuCostUnit::Worker) {
            const auto operation = input::string_value(entry.at("operation"));
            const auto part = SourceCpuWork::parse_part(entry.at("part").get<std::string>());
            if (!workers_.emplace(std::pair{ operation, part }, duration).second) throw std::invalid_argument("duplicate worker cost features");
            continue;
        }

        const auto accepted = static_cast<std::size_t>(input::integer(entry.at("accepted_requests"), 0, maximum));
        const auto running = static_cast<std::size_t>(input::integer(entry.at("running_requests"), 0, maximum));
        const auto horizon = static_cast<std::size_t>(input::integer(entry.at("horizon_checks"), 1, maximum));
        if (entry.at("request_states_initialized").get<bool>() != (accepted != 0))
            throw std::invalid_argument("candidate initialization feature disagrees with its accepted path");
        if (!admission_.emplace(std::tuple{ accepted, running, horizon }, duration).second) throw std::invalid_argument("duplicate admission cost features");
    }
}

std::optional<std::int64_t> CpuCostModel::admission_cost(const AdmissionServiceWork & work) const {
    const auto found = admission_.find({ work.accepted_requests, work.running_requests, work.horizon_checks });
    if (found == admission_.end()) return std::nullopt;
    return found->second;
}

std::optional<std::int64_t> CpuCostModel::worker_cost(const std::string & operation, CpuSubmissionPart part) const {
    const auto found = workers_.find({ operation, part });
    if (found == workers_.end()) return std::nullopt;
    return found->second;
}

std::optional<std::int64_t> CpuCostModel::estimate(const ServiceWork & work) const {
    if (work.node_kind != SourceSpanKind::CpuControl) return std::nullopt;
    if (unit_ == CpuCostUnit::Admission) {
        if (const auto member = work.admission_member()) return admission_cost(*member);
        return std::nullopt;
    }

    // The producer marks complete units; target port assembly supplies the
    // submitted work. An unresolved target root cannot authorize this estimate.
    if (!work.cpu_submission || work.cpu_submission->roots.empty()) return std::nullopt;
    const auto & observation = *work.cpu_submission->observation;
    return worker_cost(observation.operation, observation.part);
}

} // namespace markov
