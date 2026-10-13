#include "markov/performance/service_work.hpp"

#include "markov/contracts/control.hpp"
#include "markov/templates/publication_parameters.hpp"

#include <nlohmann/json.hpp>

#include <utility>

namespace markov {

nlohmann::json CpuSubmissionWork::describe() const {
    auto result = observation->describe();
    result.erase("submission"); // Source identity is not a cost matching feature.
    auto submitted = nlohmann::json::array();
    for (const auto & root : roots) {
        nlohmann::json work{
            { "kind", SourceGraph::span_name(root.kind) }
        };
        if (root.kernel) work["kernel"] = root.kernel->describe();
        if (root.communication) work["communication"] = root.communication->describe();
        if (root.transfer) work["transfer"] = root.transfer->describe();
        submitted.push_back(std::move(work));
    }
    result["submission_roots"] = std::move(submitted);
    result["target_submission_resolved"] = !roots.empty();
    return result;
}

bool AdmissionServiceWork::request_states_initialized() const {
    // The accepted path initializes its horizon state on the first candidate.
    // Later accepted members reuse that state, even when the running set is empty.
    return accepted_requests != 0;
}

nlohmann::json AdmissionServiceWork::describe() const {
    return {
        {          "accepted_requests",            accepted_requests },
        {           "running_requests",             running_requests },
        {               "input_tokens",                 input_tokens },
        {             "horizon_checks",               horizon_checks },
        { "request_states_initialized", request_states_initialized() }
    };
}

std::optional<AdmissionServiceWork> ServiceWork::admission_member() const {
    if (!admission_member_slot) return std::nullopt;

    const auto slot = *admission_member_slot;
    const auto & input = invocation.input;
    return AdmissionServiceWork{ slot,
                                 input.admission.running_requests,
                                 input.requests.at(slot).input_tokens,
                                 input.admission_work.request_horizon_checks.at(slot) };
}

nlohmann::json ServiceWork::describe() const {
    nlohmann::json result{
        {      "kind", node_kind == SourceSpanKind::CpuControl ? "cpu_service" : SourceGraph::span_name(node_kind) },
        { "node_kind",                                                           SourceGraph::span_name(node_kind) }
    };
    if (kernel) {
        result["kind"] = "compute";
        result["kernel"] = kernel->describe();
    }
    if (communication) {
        result["kind"] = "communication";
        result["communication"] = communication->describe();
    }
    if (transfer) {
        result["kind"] = "transfer";
        result["transfer"] = transfer->describe();
    }

    if (control_part) result["cpu_part"] = SourceGraph::cpu_part_name(*control_part);

    // The stage context describes the target decision. It is not a claim that
    // every service segment repeats the entire admission traversal.
    if (control_part == SourceCpuPart::SchedulePrefix) {
        const auto & input = invocation.input;
        const auto admitted = input.phase == BatchPhase::Prefill ? input.requests.size() : 0;
        result["control_context"] = {
            { "admission", input.admission_work.describe(input.admission, admitted) },
            { "admission_entry", input.admission.describe() },
            { "transition", input.transition.describe() }
        };
    }

    if (const auto member = admission_member()) result["admission_member"] = member->describe();
    if (cpu_submission) result["cpu_submission"] = cpu_submission->describe();

    // A member service carries only its own work, not the whole batch's output.
    if (publication_member_slot) result["publication_member"] = publication->members[*publication_member_slot].describe();
    else if (publication) result["publication"] = publication->describe();
    return result;
}

} // namespace markov
