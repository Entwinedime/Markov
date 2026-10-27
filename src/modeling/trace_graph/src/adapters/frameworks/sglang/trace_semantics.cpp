#include "markov/trace_graph/adapters/frameworks/sglang/trace_semantics.hpp"

namespace markov::trace_graph::adapters::sglang {

void classify_event(core::TraceEvent & event) {
    using Channel = core::TraceSourceChannel;
    using Role = core::TraceCallRole;
    using Routing = core::TraceRouting;
    const auto & name = event.name;
    event.call_role = Role::Ordinary;
    event.routing = Routing::Execution;

    if (event.cat == "hicache" || name.starts_with("HiCache::") || name.starts_with("hicache_")) {
        // Category normalization removes an enqueue role from cache markers;
        // native API anchors retain their separately interpreted identity.
        if (event.cat == "enqueue") event.submit_anchor = false;

        event.cat = "hicache";
        if (event.arg("domain") != "hicache") event.set_arg("domain", "hicache");
    }

    if (name.starts_with("hicache.control.") && !name.ends_with(".self") && event.ph == 'X') {
        event.call_role = event.source_channel == Channel::Torch && name == "hicache.control.host_load_branch" ? Role::HostLoadEnvelope : Role::ControlEnvelope;
    }
    else if (name.starts_with("runtime.phase.") || name == "runtime.hicache.extend_allocator" || name == "runtime.hicache.decode_allocator")
        event.call_role = Role::ModelWork;
    else if (name == "runtime.cpu_collective" || name == "runtime.hicache.prefetch_check" || name == "runtime.hicache.prefetch_stop"
             || name == "runtime.hicache.prefetch_enqueue" || name == "runtime.hicache.write_completion")
        event.call_role = Role::ControlBoundary;
    else if (event.source_channel == Channel::Torch && event.ph == 'X' && event.pid == event.tid && name.starts_with("step[EXTEND"))
        event.call_role = Role::PrefillMarker;
    else if (event.source_channel == Channel::Torch && event.ph == 'X' && event.pid == event.tid && name.starts_with("step[DECODE"))
        event.call_role = Role::DecodeMarker;
    event.snapshot_overhead = name.ends_with(":state_snapshot") || (event.source_channel == Channel::PythonProbe && event.has_arg_key_hint("state_snapshot"));

    if (event.source_channel == Channel::PythonProbe && event.cat == "runtime_diagnostic") {
        if (name == "runtime.triton.prepare" || name == "runtime.triton.load" || name == "runtime.allocator.kernel" || name == "runtime.cpu_collective"
            || name.starts_with("runtime.phase.") || name == "runtime.hicache.layer_waits" || name == "runtime.hicache.prefetch_check"
            || name == "runtime.hicache.prefetch_progress" || name == "runtime.hicache.write_policy_check" || name == "runtime.hicache.radix_insert"
            || name == "runtime.hicache.node_publish" || name == "runtime.hicache.device_restore" || name == "runtime.hicache.prefetch_stop"
            || name == "runtime.hicache.prefetch_publish" || name == "runtime.hicache.prefetch_read" || name == "runtime.hicache.prefetch_query"
            || name == "runtime.hicache.host_release" || name == "runtime.hicache.storage_drain" || name == "runtime.hicache.prefetch_enqueue"
            || name == "runtime.hicache.load_completion" || name == "runtime.hicache.write_completion" || name == "runtime.hicache.write_ack"
            || name == "runtime.hicache.decode_allocation" || name == "runtime.hicache.extend_allocation" || name == "runtime.hicache.decode_allocator"
            || name == "runtime.hicache.extend_allocator" || name == "runtime.hicache.capacity_guard" || name == "runtime.hicache.load_allocation"
            || name == "runtime.hicache.load_admission" || name == "runtime.hicache.host_load_check" || name == "runtime.hicache.device_release_backup"
            || name == "runtime.hicache.allocator_free" || name == "runtime.hicache.device_release_regular")
            event.routing = Routing::RuntimeObservation;
        else if (name == "runtime.request.receive") event.routing = Routing::RequestBegin;
        else if (name == "runtime.response.scheduler_send" || name == "runtime.request.socket_received" || name == "runtime.request.dispatch_ready")
            event.routing = Routing::RequestObservation;
        else event.routing = Routing::Ignore;
    }
    else if (event.source_channel == Channel::PythonProbe && event.has_arg_key_hint("fact")) event.routing = Routing::CacheFact;
}

} // namespace markov::trace_graph::adapters::sglang
