#include "markov/adapters/sglang/observations.hpp"

#include <utility>
#include <vector>

namespace markov::adapters::sglang {

using namespace trace_graph::source;

void ProbeReader::record(std::size_t position, const nlohmann::json & event) {
    const auto phase = event.at("ph").get<std::string>();
    ++output_.counts["probe_phase." + phase];
    if (phase != "X") return;

    const auto name = event.at("name").get<std::string>();
    const auto & args = event.at("args");
    const auto begin = nanoseconds_from_us(event.at("ts"));
    const auto end = begin + nanoseconds_from_us(event.at("dur"));
    const RecordRef reference{ file_id_, position };
    const auto process = scalar_identity(event.at("pid"));
    if (name == "hicache_cache_extend_input_start") forward_phases_[process] = "prefill";
    if (name == "runtime.hicache.decode_allocation") forward_phases_[process] = "decode";

    const bool received = name == "runtime.request.socket_received";
    const bool sent = name == "runtime.response.scheduler_send";
    if (received || sent || name == "runtime.phase.linear") {
        const auto thread = scalar_identity(event.at("tid"));
        const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + process + ":" + thread);
        if (name == "runtime.phase.linear") observe_linear(args, process, resource, begin, end, reference);
        else {
            const auto point = output_.point({ sent ? end : begin, resource, output_.name(name), reference });
            for (const auto & identity : args.at("request_ids")) {
                auto & ports = requests[identity.get<std::string>()];
                (sent ? ports.sent : ports.received).push_back(point);
            }
        }
    }

    if (args.contains("fact") || name.starts_with("runtime.")) { ++roles[name]; }
}

void ProbeReader::observe_linear(const nlohmann::json & args, const std::string & process, Index resource, Time begin, Time end, RecordRef record) {
    if (args.at("status") != "returned") return;
    const auto phase = forward_phases_.find(process);
    if (phase == forward_phases_.end()) {
        ++output_.counts["linear_work_without_forward_context"];
        return;
    }

    const auto input = args.at("input_shape").get<std::vector<double>>();
    const auto weight = args.at("weight_shape").get<std::vector<double>>();
    const auto input_stride = args.at("input_stride").get<std::vector<double>>();
    const auto weight_stride = args.at("weight_stride").get<std::vector<double>>();
    if (input.size() != 2 || weight.size() != 2 || input_stride.size() != 2 || weight_stride.size() != 2) {
        ++output_.counts["linear_work_unsupported_geometry"];
        return;
    }
    const auto m = input[0], k = input[1], n = weight[0];
    auto implementation = "torch.linear:" + args.at("input_dtype").get<std::string>();
    implementation += args.at("bias_shape").is_null() ? ":no_bias" : ":bias";
    if (args.at("input_dtype") != args.at("weight_dtype")) implementation += ":weight=" + args.at("weight_dtype").get<std::string>();

    // The probe supplies geometry and provenance, not a device measurement.
    // The independent provider estimates service; this CPU envelope is not charged.
    trace_graph::planning::Work work{
        { phase->second + ".linear", std::move(implementation), "device_service" },
        { { "m", m },
         { "n", n },
         { "k", k },
         { "multiply_accumulate_count", m * n * k },
         { "input_elements", m * k },
         { "output_elements", m * n },
         { "weight_elements", n * k },
         { "input_stride_0", input_stride[0] },
         { "input_stride_1", input_stride[1] },
         { "weight_stride_0", weight_stride[0] },
         { "weight_stride_1", weight_stride[1] } }
    };
    const auto description = output_.describe_work(std::move(work));
    output_.work_observations.push_back({ description, resource, begin, end, record });
}

} // namespace markov::adapters::sglang
