#include "markov/adapters/sglang/observations.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace markov::adapters::sglang {

ProbeReader::ProbeReader(trace_graph::source::Observations & output, std::optional<bool> deterministic_sampling,
                         const std::optional<std::filesystem::path> & configuration)
    : adapters::ProbeReader(output),
      configuration_(configuration),
      deterministic_sampling_(deterministic_sampling) {}

void ProbeReader::observe_response(const ProbeEvent & event) {
    using namespace trace_graph;
    if (!configuration_) return;

    const auto & fields = event.args;
    if (fields.at("status") != "returned") throw std::runtime_error("Cannot evaluate a failed regional observation");
    const auto requests = fields.at("request_ids").get<std::vector<std::string>>();
    if (requests.size() != 1) throw std::runtime_error("Regional cost evaluation currently supports serial single-request frames");

    auto features = fields.at("features").get<planning::Features>();
    auto operation = event.name.substr(std::string("region.").size());
    const bool stream = operation == "stream" || operation == "prepare_stream";
    if (stream) operation = operation == "stream" ? "batch" : "prepare";
    const auto scope = stream ? "current_stream_interval" : "thread_cpu";
    if (operation == "result" && result_implementation_.empty()) {
        std::ifstream stream(*configuration_);
        const auto settings = nlohmann::json::parse(stream).at("server").at("arguments");
        result_implementation_ = std::string("sglang.ascend.eager:") + (settings.value("enable_hierarchical_cache", false) ? "HiRadixCache" : "RadixCache");
    }
    const auto implementation = operation == "result" ? result_implementation_ : fields.at("implementation").get<std::string>();
    const auto phase = fields.at("phase").get<std::string>();
    const auto request = output_.name(requests.front());
    const auto iteration = fields.at("iteration").get<std::size_t>();
    const auto participant = fields.at("participant").get<std::size_t>();
    const auto resource = output_.resource(source::ResourceKind::CpuThread, "cpu:" + event.process + ":" + event.thread);
    auto region = source::ObservedRegion{ resource, event.begin_ns, event.end_ns, event.record };
    const auto identity = std::tuple(request, iteration, participant);
    if (operation == "batch" && !stream) {
        if (fields.contains("input_metadata")) region_batches_.emplace(identity, region);
        return;
    }
    if (operation == "load_failed") {
        const auto cost = fields.at("thread_cpu_ns").get<source::Time>() - fields.at("delegated_cpu_ns").get<source::Time>();
        if (cost < 0) throw std::runtime_error("Delegated backup CPU exceeds failed load CPU");
        output_.cost_observations.push_back({
            request,
            iteration,
            participant,
            "load_failed",
            { { "cache.load.failed", "sglang.HiRadixCache.init_load_back", "thread_cpu" }, std::move(features) },
            cost,
            { resource, event.begin_ns, event.end_ns, event.record },
            std::nullopt,
            false
        });
        return;
    }
    const auto duration = fields.at(stream ? "stream_interval_ns" : "thread_cpu_ns").get<source::Time>();
    if (duration < 0) throw std::runtime_error("Regional response duration must be nonnegative");
    const auto add = [&](std::string name, std::string timing, source::Time cost, bool contiguous = true) {
        planning::Work work{
            { name + "." + phase, implementation, timing },
            features
        };
        output_.cost_observations.push_back({ request,
                                              iteration,
                                              participant,
                                              operation == "result"  ? "result"
                                              : operation == "batch" ? "batch"
                                                                     : "prepare",
                                              std::move(work),
                                              cost,
                                              region,
                                              std::nullopt,
                                              contiguous });
    };
    if (operation == "result" && fields.contains("send_cpu_ns")) {
        const auto send = fields.at("send_cpu_ns").get<source::Time>();
        if (send < 0 || send > duration) throw std::runtime_error("Result send must lie within its enclosing CPU interval");
        add("result_send", scope, send);
        add("result_tail", scope, duration - send);
    }
    else add(operation, scope, duration);
    if (!stream && operation != "result") add(operation, "caller_blocking_interval", event.end_ns - event.begin_ns);
    if (operation == "batch" && stream) {
        const auto batch = region_batches_.find(identity);
        if (batch == region_batches_.end()) return;
        region = batch->second;
        region_batches_.erase(batch);
        if (!fields.contains("input_metadata") || !fields.at("input_metadata").contains("stream_interval_ns")) return;

        const auto & metadata = fields.at("input_metadata");
        const auto device = metadata.at("stream_interval_ns").get<source::Time>();
        const auto cpu =
            fields.at("thread_cpu_ns").get<source::Time>() - std::max(source::Time{ 0 }, metadata.at("thread_cpu_ns").get<source::Time>() - device);
        const auto caller = region.end_ns - region.begin_ns - std::max(source::Time{ 0 }, metadata.at("wall_ns").get<source::Time>() - device);
        if (device < 0 || cpu < 0 || caller < 0) throw std::runtime_error("Input metadata exceeds its batch interval");

        // The enclosing batch bounds order the response, but removed waits
        // leave noncontiguous CPU work, not a newly observed service interval.
        add("batch", "input_ready_thread_cpu", cpu, false);
        add("batch", "input_ready_caller_interval", caller, false);
    }
}

} // namespace markov::adapters::sglang
