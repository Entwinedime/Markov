#pragma once

#include "markov/trace_graph/structure/operation.hpp"

#include <string>

namespace markov::trace_graph::structure {

enum class DeviceEventKind { Record, Wait };

/** One Ascend device event instruction, independent of its caller. CPU enqueue,
 * worker dispatch and dependency waiting are not part of this service. The
 * deployment still limits hardware/runtime applicability; sharing the primitive
 * does not imply that caller costs or stream scheduling are interchangeable. */
[[nodiscard]] inline OperationWork ascend_event_work(const std::string & component, DeviceEventKind kind) {
    return { component,
             kind == DeviceEventKind::Wait ? "device.event.wait" : "device.event.record",
             kind == DeviceEventKind::Wait ? "ascend.EVENT_WAIT" : "ascend.EVENT_RECORD",
             "device_service",
             {} };
}

} // namespace markov::trace_graph::structure
