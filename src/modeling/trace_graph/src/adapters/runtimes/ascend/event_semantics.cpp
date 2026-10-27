#include "markov/trace_graph/adapters/runtimes/ascend/event_semantics.hpp"

namespace markov::trace_graph::adapters::ascend {

void classify_event(core::TraceEvent & event) {
    using Boundary = core::TraceBoundary;
    const auto & name = event.name;
    event.submit_anchor = name.starts_with("Enqueue@") || name == "Node@launch" || event.cat == "enqueue" || name.starts_with("AscendCL@aclrtLaunch")
                          || name.starts_with("AscendCL@aclrtMemcpyAsync") || name == "AscendCL@aclrtMemcpy2dAsync" || name == "AscendCL@aclrtRecordEvent"
                          || name == "AscendCL@aclrtWaitEvent";
    event.collective_envelope = name.contains("hcom") || name.contains("HCCL") || name.contains("hccl");

    if (name == "EVENT_RECORD") event.boundary = Boundary::DeviceEventRecord;
    else if (name == "EVENT_WAIT") event.boundary = Boundary::DeviceEventWait;
    else if (name == "AscendCL@aclrtStreamWaitEvent") event.boundary = Boundary::StreamWait;
    else if (name == "AscendCL@aclrtSynchronizeStream" || name == "AscendCL@aclrtSynchronizeStreamWithTimeout") event.boundary = Boundary::StreamSync;
    else if (name == "AscendCL@aclrtSynchronizeEvent" || name == "AscendCL@aclrtSynchronizeEventWithTimeout") event.boundary = Boundary::EventSync;
    else if (name == "AscendCL@aclrtSynchronizeDevice" || name == "AscendCL@aclrtSynchronizeDeviceWithTimeout") event.boundary = Boundary::DeviceSync;
    else if (name == "NOTIFY_RECORD") event.boundary = Boundary::NotifyRecord;
    else if (name == "NOTIFY_WAIT") event.boundary = Boundary::NotifyWait;
    else if (name == "MODEL_EXECUTE") event.boundary = Boundary::ModelExecute;
    else if (name == "AscendCL@aclrtRecordEvent") event.boundary = Boundary::HostEventRecord;
    else if (name == "AscendCL@aclrtMemcpyAsync" || name == "AscendCL@aclrtMemcpy2dAsync") event.boundary = Boundary::AsyncCopy;
    else if (name == "MEMCPY_ASYNC") event.boundary = Boundary::DeviceCopy;
    else if (name == "Node@launch") event.boundary = Boundary::NodeLaunch;
    else if (name.starts_with("Runtime@")) event.boundary = Boundary::RuntimeDetail;
    else event.boundary = Boundary::None;
}

} // namespace markov::trace_graph::adapters::ascend
