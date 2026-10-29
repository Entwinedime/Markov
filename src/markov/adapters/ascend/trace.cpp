#include "markov/adapters/ascend/trace.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace markov::adapters::ascend {

using namespace trace_graph::source;

TraceReader::TraceReader(Observations & output, RuntimeCausality & causality, const ProfileFile & file, std::size_t file_id)
    : output_(output),
      causality_(causality),
      file_(file),
      file_id_(file_id) {
    if (!file.clock_recorded) throw std::runtime_error("Ascend trace requires a recorded host clock transform: " + file.path.string());
}

Index TraceReader::lane(const std::string & process, const std::string & thread, bool device) {
    if (device) {
        if (const auto found = device_lanes_.find(thread); found != device_lanes_.end()) return found->second;

        const auto id = output_.resource(ResourceKind::DeviceStream, "device:" + std::to_string(file_id_) + ":" + thread);
        device_lanes_[thread] = id;
        return id;
    }

    if (const auto found = cpu_lanes_.find({ process, thread }); found != cpu_lanes_.end()) return found->second;

    const auto prefix = file_.channel == Channel::Torch ? "display:" + std::to_string(file_id_) + ":" : "cpu:";
    const auto id = output_.resource(ResourceKind::CpuThread, prefix + process + ":" + thread);
    cpu_lanes_[{ process, thread }] = id;
    return id;
}

void TraceReader::record(std::size_t position, const nlohmann::json & event) {
    const auto phase = event.at("ph").get<std::string>();
    ++output_.counts["phase." + phase];
    const auto process = scalar_identity(event.at("pid"));
    const auto thread = scalar_identity(event.at("tid"));
    const auto name = event.value("name", std::string{});
    static const auto empty_args = nlohmann::json::object();
    const auto & args = event.contains("args") ? event.at("args") : empty_args;
    if (phase == "M") {
        if (name == "process_name" && args.value("name", std::string{}) == "CANN") runtime_processes_.insert(process);
        return;
    }
    if (phase != "X" && phase != "s" && phase != "f") return;

    const auto category = event.value("cat", std::string{});
    // Native flow destinations are device ports even when the exporter uses a
    // different display PID from its hardware duration records.
    const bool device_flow = phase == "f" && (category == "async_npu" || category == "HostToDevice");
    const bool device = device_flow || args.contains("Physic Stream Id") || ((category == "Kernel" || category == "cpu_op") && args.contains("streamId"));
    if (device) device_processes_.insert(process);
    if (file_.channel == Channel::Torch && !device && (category == "cpu_op" || category == "enqueue" || category == "dequeue")) host_processes_.insert(process);

    const auto begin = nanoseconds_from_us(event.at("ts")) + file_.clock_offset_ns;
    const auto resource = lane(process, thread, device);
    // Per-call flow IDs stay in provenance, not millions of display labels.
    const auto name_id = output_.name(category == "HostToDevice" ? "HostToDevice" : name);
    const RecordRef reference{ file_id_, position };
    if (phase == "s" || phase == "f") {
        const auto point = output_.point({ begin, resource, name_id, reference });
        const auto native_id = scalar_identity(event.at("id"));
        auto identity = std::to_string(file_id_) + ":" + category + ":" + native_id;
        if (category == "async_npu" && name == "torch_to_npu") {
            // Torch-NPU create_torch_to_npu_flow emits adjacent s/f records.
            // Its ID is a kernel timestamp and can collide across streams.
            if (phase == "s") {
                identity = std::to_string(file_id_) + ":kernel_flow:" + std::to_string(position);
                kernel_flow_ = ExportedKernelFlow{ native_id, identity, position };
            }
            else if (kernel_flow_ && kernel_flow_->position + 1 == position && kernel_flow_->native_id == native_id) {
                identity = kernel_flow_->identity;
                kernel_flow_.reset();
                ++output_.counts["native_kernel_flow_pairs"];
            }
            else ++output_.counts["unresolved_native_kernel_flow_pair"];
        }
        const auto kind = category == "HostToDevice" || category == "async_task_queue" ? DependencyKind::Submission : DependencyKind::Flow;
        output_.flows.push_back({ std::move(identity), point, phase == "s", kind });
        return;
    }

    const auto duration = nanoseconds_from_us(event.at("dur"));
    if (duration > std::numeric_limits<Time>::max() - begin) throw std::runtime_error("Trace end timestamp overflows nanoseconds");
    ObservedSpan span{ begin, begin + duration, resource, name_id, reference };
    const bool code_preparation =
        name == "AscendCL@aclrtBinaryLoadFromData" || name == "Runtime@BinaryGetFunctionByEntry" || name == "Runtime@BinaryGetFunctionByName";
    const bool device_submission = name == "AscendCL@aclrtLaunchKernelWithHostArgs" || name == "AscendCL@aclrtMemcpyAsync";
    if (!device && file_.channel == Channel::Torch && (name.starts_with("AscendCL@") || code_preparation))
        output_.native_runtime_calls.push_back({
            output_.name(name.substr(name.starts_with("AscendCL@") ? 9 : 8)),
            { resource, begin, begin + duration, reference },
            code_preparation,
            device_submission
        });
    if ((category == "enqueue" && name.starts_with("Enqueue@")) || (category == "dequeue" && name.starts_with("Dequeue@"))) {
        const auto identity = scalar_identity(args.at("correlation_id"));
        auto & pair = queue_calls_[{ process, identity }];
        auto & destination = category == "enqueue" ? pair.submitted : pair.executed;
        if (destination) pair.ambiguous = true;
        destination = span;
    }
    if (file_.channel == Channel::NativeHook && args.contains("return_code") && args.at("return_code") != 0) ++output_.counts["ascend.failed_native_call"];
    else causality_.observe(event, span, device, file_.channel == Channel::NativeHook);
    output_.span(span);
}

void TraceReader::finish() {
    for (const auto & [identity, pair] : queue_calls_) {
        if (pair.ambiguous || !pair.submitted || !pair.executed) {
            ++output_.counts[pair.ambiguous ? "native_queue.ambiguous" : "native_queue.unpaired"];
            continue;
        }
        const auto & submitted = *pair.submitted;
        const auto & executed = *pair.executed;
        const auto native_name = output_.names[submitted.name].substr(8);
        if (native_name != output_.names[executed.name].substr(8) || submitted.begin_ns > executed.begin_ns) {
            ++output_.counts["native_queue.inconsistent"];
            continue;
        }
        output_.native_queue_calls.push_back({
            output_.name(native_name),
            { submitted.resource, submitted.begin_ns, submitted.end_ns, submitted.record },
            {  executed.resource,  executed.begin_ns,  executed.end_ns,  executed.record }
        });
        ++output_.counts["native_queue.paired"];
    }

    // Profiler process IDs are display domains. Merge only a proven single
    // host process; otherwise preserve lanes and expose the ambiguity.
    if (host_processes_.size() == 1) {
        const auto & host = *host_processes_.begin();
        for (const auto & [identity, id] : cpu_lanes_) {
            if (runtime_processes_.contains(identity.first) || host_processes_.contains(identity.first)) {
                const auto process = runtime_processes_.contains(identity.first) ? host : identity.first;
                const auto target = output_.resource(ResourceKind::CpuThread, "cpu:" + process + ":" + identity.second);
                output_.alias_resource(id, target);
            }
        }
    }
    else if (!runtime_processes_.empty()) { ++output_.counts["ambiguous_runtime_host_process"]; }

    // Flow endpoints lack device attributes; resolve their display process
    // against device spans after the complete file has supplied that evidence.
    for (const auto & [identity, id] : cpu_lanes_) {
        if (!device_processes_.contains(identity.first)) continue;
        const auto stream = device_lanes_.find(identity.second);
        if (stream != device_lanes_.end()) output_.alias_resource(id, stream->second);
    }
}

} // namespace markov::adapters::ascend
