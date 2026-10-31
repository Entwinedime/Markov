#include "markov/adapters/ktransformers/observations.hpp"
#include "markov/adapters/ktransformers/work.hpp"

#include <fstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace markov::adapters::ktransformers {

ProbeReader::ProbeReader(trace_graph::source::Observations & output, const std::optional<std::filesystem::path> & configuration)
    : adapters::ProbeReader(output) {
    if (!configuration) return;

    std::ifstream stream(*configuration);
    configuration_ = nlohmann::json::parse(stream).at("server").at("arguments");
}

void ProbeReader::observe(const ProbeEvent & event) {
    using namespace trace_graph::source;
    if (event.name != "runtime.ktransformers.phase" && event.name != "runtime.ktransformers.generate") return;

    const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + event.process + ":" + event.thread);
    const auto name = output_.name(event.name);
    const ObservedRegion region{ resource, event.begin_ns, event.end_ns, event.record };
    output_.span({ event.begin_ns, event.end_ns, resource, name, event.record });
    const auto & args = event.args;
    const auto requests = args.at("request_ids").get<std::vector<std::string>>();
    if (requests.size() != 1) throw std::runtime_error("Synchronous KTransformers regions require one request");
    const auto & identity = requests.front();
    const auto request = output_.name(identity);
    const auto duration = event.end_ns - event.begin_ns;
    if (event.name == "runtime.ktransformers.phase") {
        // Dense phases explicitly call torch.npu.synchronize before recording
        // their end. The source DAG must retain that observed return dependency.
        output_.device_completion_fences.push_back({ region, output_.point({ event.end_ns, resource, name, event.record }) });
    }
    if (event.name == "runtime.ktransformers.phase" && configuration_) {
        auto work = phase_work(*configuration_,
                               args.at("phase").get<std::string>(),
                               args.at("query_tokens").get<Index>(),
                               args.at("context_tokens").get<Index>(),
                               args.at("first_phase").get<bool>(),
                               args.at("emit_token").get<bool>());
        output_.cost_observations.push_back({ request, iterations_[identity]++, 0, "phase", std::move(work), duration, region });
        phase_durations_[identity] += duration;
    }
    if (event.name == "runtime.ktransformers.generate") {
        output_.request_executions.push_back({ request, region });
        if (!configuration_) return;
        const auto remainder = duration - phase_durations_.at(identity);
        if (remainder < 0) throw std::runtime_error("Generate phases exceed their enclosing interval");
        auto work = control_work(*configuration_,
                                 args.at("input_tokens").get<Index>(),
                                 args.at("prefill_steps").get<Index>(),
                                 args.at("decode_steps").get<Index>(),
                                 args.at("first_request").get<bool>());
        output_.cost_observations.push_back({ request, 0, 0, "control", std::move(work), remainder, region });
    }
}

} // namespace markov::adapters::ktransformers
