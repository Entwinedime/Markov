#include "markov/trace_graph/cost/hicache_submission.hpp"
#include "markov/trace_graph/cost/host_operation.hpp"
#include "markov/trace_graph/structure/ascend_event.hpp"
#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace markov::trace_graph::cost {
namespace {

PageFirstSubmissionCost estimate_submission(OperatorCostProvider & provider, structure::OperationWork work, bool load) {
    using Primitive = HostOperationCost;
    const auto primitive = [&](std::string_view operation, bool submitted) {
        work.operation = std::string(load ? "load.submission." : "write.submission.") + std::string(operation);
        std::optional<structure::OperationWork> device_work;
        if (submitted)
            device_work =
                structure::ascend_event_work(work.component, operation == "wait_event" ? structure::DeviceEventKind::Wait : structure::DeviceEventKind::Record);
        return estimate_host_operation(provider, work, submitted, std::move(device_work));
    };

    // Waits are structural. Use independently measured idle API overhead,
    // followed by the observed host-clock gap after synchronization returns.
    // Blocking API wall time is neither preparation nor CPU service.
    auto synchronization = work;
    synchronization.operation = std::string(load ? "load.submission." : "write.submission.") + "index_sync";
    synchronization.timing_scope = "residual_interval";
    const std::array synchronization_work{
        structure::OperationWork{ .component = work.component,
                                 .operation = "runtime.stream_sync",
                                 .implementation = "ascend.runtime",
                                 .timing_scope = "idle_stream_sync_interval",
                                 .features = { { "previous_work_completed", 1.0 } } },
        synchronization,
    };
    const auto synchronized = provider.estimate_all(synchronization_work);
    const Primitive sync_cost{ .main_us = synchronized[0].duration_us, .main_residual_us = synchronized[1].duration_us };

    return { primitive("prepare_sync", false),
             sync_cost,
             primitive("start_record", true),
             primitive("wait_event", true),
             primitive("first_copy", false),
             primitive("copy", false),
             primitive(load ? "first_layer_record" : "completion_record", true),
             load && work.features.at("layers") > 1 ? primitive("layer_record", true) : Primitive{},
             primitive("tail", false) };
}

} // namespace

PageFirstSubmissionCost estimate_write_submission(OperatorCostProvider & provider, uint64_t payload_bytes, uint64_t page_bytes, bool write_back) {
    return estimate_submission(provider,
                               {
                                   "hicache_io_control",
                                   "",
                                   write_back ? "kernel_ascend.page_first_direct.write_back" : "kernel_ascend.page_first_direct.write_through",
                                   "",
                                   { { "payload_bytes", static_cast<double>(payload_bytes) },
                                     { "page_bytes", static_cast<double>(page_bytes) },
                                     { "pages", static_cast<double>(payload_bytes / page_bytes) } }
    },
                               false);
}

PageFirstSubmissionCost estimate_load_submission(OperatorCostProvider & provider, uint64_t payload_bytes, uint64_t page_bytes, size_t layers) {
    return estimate_submission(provider,
                               {
                                   "hicache_io_control",
                                   "",
                                   "kernel_ascend.page_first_direct.load",
                                   "",
                                   { { "payload_bytes", static_cast<double>(payload_bytes) },
                                     { "page_bytes", static_cast<double>(page_bytes) },
                                     { "pages", static_cast<double>(payload_bytes / page_bytes) },
                                     { "layers", static_cast<double>(layers) } }
    },
                               true);
}

} // namespace markov::trace_graph::cost
