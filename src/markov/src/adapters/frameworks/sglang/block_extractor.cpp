#include "markov/adapters/frameworks/sglang/block_extractor.hpp"

#include "markov/adapters/frameworks/sglang/bindings.hpp"

#include <optional>
#include <stdexcept>
#include <string>

namespace markov {

nlohmann::json SGLangBlockExtractor::resolve_ports(std::size_t index) const {
    const auto & call = calls_[index];
    nlohmann::json ports = nlohmann::json::object();
    if (call.marker) {
        const auto & marker = graph_.spans()[*call.marker];
        ports["entered"] = marker.begin;
        ports["native_returned"] = marker.end;
        if (call.step == StepKind::Prepare) ports["prepare_body_returned"] = marker.end;
    }
    if (!call.binding) return ports;

    const SGLangBindings bindings;
    const auto & binding = bindings.binding(*call.binding);
    if (binding.step != call.step || (binding.phase && call.phase != binding.phase)) {
        throw std::invalid_argument("observed block disagrees with its native binding: " + call.identity);
    }

    for (const auto & port : binding.ports) {
        std::optional<BoundaryId> boundary;
        switch (port.anchor) {
        case NativePortAnchor::ScopeReturn:
            if (call.marker) boundary = graph_.spans()[*call.marker].end;
            break;
        case NativePortAnchor::OwnedDeviceCompletion:
            boundary = call.device_done;
            break;
        case NativePortAnchor::ScheduleDecision:
            // An empty scheduling decision completes at return; a selected
            // batch becomes visible before its nested preparation starts.
            if (!call.batch_id && call.marker) {
                boundary = graph_.spans()[*call.marker].end;
                break;
            }
            for (const auto & child : calls_) {
                if (child.is_annotation() || child.serving_parent(calls_) != index || child.step != StepKind::Prepare || !child.marker
                    || child.rank != call.rank || child.batch_id != call.batch_id)
                    continue;
                if (boundary) throw std::invalid_argument("ambiguous selection anchor: " + call.identity);
                boundary = graph_.spans()[*child.marker].begin;
            }
            break;
        case NativePortAnchor::ParentScheduleReturn:
            // Prepared includes the enclosing scheduler's suffix, not just the body.
            if (const auto schedule = call.serving_parent(calls_)) {
                const auto & parent = calls_[*schedule];
                if (parent.step == StepKind::Schedule && parent.rank == call.rank && parent.batch_id == call.batch_id && parent.marker) {
                    boundary = graph_.spans()[*parent.marker].end;
                }
            }
            break;
        }

        if (boundary) ports[std::string(ServingContract{}.port_name(port.port))] = *boundary;
    }

    return ports;
}

} // namespace markov
