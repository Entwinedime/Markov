#include "markov/adapters/frameworks/sglang/bindings.hpp"
#include "markov/graph/source_graph.hpp"
#include "markov/production/source_call.hpp"

#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace markov {

std::optional<std::string> SGLangBindings::cpu_name(const SourceCall & call) const {
    if (call.kind != SourceCallKind::NativeRegion || !call.binding) return std::nullopt;
    return region(*call.binding).cpu_name();
}

bool SGLangBindings::cpu_work_boundary(const SourceCall & call) const {
    if (call.kind != SourceCallKind::NativeRegion || !call.binding) return false;
    const auto semantic = region(*call.binding).semantic;
    return semantic == "logical_collective" || semantic == "input_request" || semantic == "input_message" || semantic == "input_message_submit"
           || semantic == "input_message_wait";
}

std::map<PortKind, BoundaryId> SGLangBindings::ports(const SourceGraph & graph, std::span<const SourceCall> calls, std::size_t index) const {
    const auto & call = calls[index];
    std::map<PortKind, BoundaryId> ports;
    if (!call.binding) return ports;

    const auto & binding = this->binding(*call.binding);
    if (binding.step != call.step || (binding.phase && call.phase != binding.phase)) {
        throw std::invalid_argument("observed block disagrees with its native binding: " + call.identity);
    }

    for (const auto & port : binding.ports) {
        std::optional<BoundaryId> boundary;
        switch (port.anchor) {
        case NativePortAnchor::ScopeReturn:
            if (call.marker) boundary = graph.spans()[*call.marker].end;
            break;
        case NativePortAnchor::OwnedDeviceCompletion:
            boundary = call.device_done;
            break;
        case NativePortAnchor::ScheduleDecision:
            // An empty scheduling decision completes at return; a selected
            // batch becomes visible before its nested preparation starts.
            if (!call.batch_id && call.marker) {
                boundary = graph.spans()[*call.marker].end;
                break;
            }
            for (const auto & child : calls) {
                if (child.is_annotation() || child.serving_parent(calls) != index || child.step != StepKind::Prepare || !child.marker || child.rank != call.rank
                    || child.batch_id != call.batch_id)
                    continue;
                if (boundary) throw std::invalid_argument("ambiguous selection anchor: " + call.identity);
                boundary = graph.spans()[*child.marker].begin;
            }
            break;
        case NativePortAnchor::ParentScheduleReturn:
            // Prepared includes the enclosing scheduler's suffix, not just the body.
            if (const auto schedule = call.serving_parent(calls)) {
                const auto & parent = calls[*schedule];
                if (parent.step == StepKind::Schedule && parent.rank == call.rank && parent.batch_id == call.batch_id && parent.marker) {
                    boundary = graph.spans()[*parent.marker].end;
                }
            }
            break;
        }

        if (boundary) ports.emplace(port.port, *boundary);
    }

    return ports;
}

} // namespace markov
