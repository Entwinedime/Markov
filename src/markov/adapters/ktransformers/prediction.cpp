#include "markov/adapters/ktransformers/prediction.hpp"
#include "markov/adapters/ktransformers/work.hpp"
#include "markov/trace_graph/source/regions.hpp"

#include <algorithm>
#include <cstdint>
#include <set>
#include <stdexcept>

namespace markov::adapters::ktransformers {
namespace {

using Json = nlohmann::json;
using trace_graph::planning::Work;

std::size_t positive(const Json & value, const char * name) {
    if (!value.is_number_integer() || value.get<std::int64_t>() <= 0) throw std::runtime_error(std::string(name) + " must be positive");
    return value.get<std::size_t>();
}

} // namespace

std::vector<SerialPredictionTarget> prepare_targets(const Json & request, const Json & capture, const trace_graph::source::SourceContext & source) {
    const auto & deployment = request.at("deployment");
    if (request.at("kernel_cache_initial_state") != "empty")
        throw std::runtime_error("KTransformers capture starts a fresh process; prepared initialization is not qualified");
    if (deployment.at("tensor_parallel") != 1 || (deployment.at("dtype") != "float16" && deployment.at("dtype") != "bfloat16"))
        throw std::runtime_error("Dense KTransformers prediction supports single-device float16/bfloat16");
    const auto & workload = request.at("workload");
    const auto & captured_workload = capture.at("workload");
    if (workload.value("drain_before_formal", false)) throw std::runtime_error("KTransformers has no HiCache preparation gate");
    const auto & observed = source.observations();
    if (workload.at("requests").size() != observed.requests.size() || captured_workload.at("requests").size() != observed.requests.size()
        || workload.at("formal_begin_index") != captured_workload.at("formal_begin_index"))
        throw std::runtime_error("Requested workload differs from the base capture");
    for (std::size_t id = 0; id < observed.requests.size(); ++id) {
        const auto & expected = workload.at("requests").at(id);
        const auto & actual = captured_workload.at("requests").at(id);
        const bool same_input =
            expected.at("input_ids").is_null() ? expected.at("prompt") == actual.at("prompt") : expected.at("input_ids") == actual.at("input_ids");
        if (!same_input || !expected.at("ignore_eos").get<bool>() || expected.at("return_logprob").get<bool>()
            || expected.at("max_new_tokens") != actual.at("max_new_tokens") || expected.at("ignore_eos") != actual.at("ignore_eos")
            || observed.requests[id].formal != (id >= workload.at("formal_begin_index").get<std::size_t>()))
            throw std::runtime_error("KTransformers prediction needs the same fixed-token workload and formal boundary");
        const auto & ports = source.regions().request(id);
        if (!ports.before_execution || !ports.after_execution) throw std::runtime_error("Base needs a complete generate region and request transport ports");
    }

    const auto & base = capture.at("server").at("arguments");
    const std::set<std::string> variable{ "attention_backend", "cpu_threads", "max_context_tokens", "prefill_chunk_tokens" };
    std::vector<SerialPredictionTarget> targets;
    for (const auto & raw : request.at("targets")) {
        auto configuration = base;
        for (const auto & [name, value] : raw.at("configuration").items()) {
            if (!variable.contains(name) && (!base.contains(name) || base.at(name) != value))
                throw std::runtime_error("Unsupported KTransformers target setting: " + name);
            configuration[name] = value;
        }
        if (configuration.at("attention_backend") != "eager" && configuration.at("attention_backend") != "sdpa")
            throw std::runtime_error("KTransformers attention_backend must be eager or sdpa");
        positive(configuration.at("cpu_threads"), "cpu_threads");
        const auto capacity = positive(configuration.at("max_context_tokens"), "max_context_tokens");
        const auto chunk =
            configuration.at("prefill_chunk_tokens").is_null() ? std::size_t{ 0 } : positive(configuration.at("prefill_chunk_tokens"), "prefill_chunk_tokens");
        auto & target = targets.emplace_back(SerialPredictionTarget{ raw.at("name").get<std::string>(), std::move(configuration), {}, std::nullopt });
        bool first_prefill = true, first_decode = true;
        for (std::size_t id = 0; id < observed.requests.size(); ++id) {
            const auto input = captured_workload.at("requests").at(id).at("input_ids").size();
            const auto output = positive(workload.at("requests").at(id).at("max_new_tokens"), "max_new_tokens");
            if (input > capacity || output > capacity - input) {
                target.infeasible = "Request " + std::to_string(id) + " exceeds target max_context_tokens";
                target.requests.clear();
                break;
            }
            const auto width = chunk ? chunk : input;
            const auto prefill_steps = (input + width - 1) / width;
            const auto & ports = source.regions().request(id);
            auto & program = target.requests.emplace_back(trace_graph::planning::SerialRequest{ id, *ports.before_execution, *ports.after_execution, {} });
            program.work.push_back(control_work(target.configuration, input, prefill_steps, output - 1, id == 0));
            for (std::size_t begin = 0; begin < input;) {
                const auto query = std::min(width, input - begin);
                begin += query;
                program.work.push_back(phase_work(target.configuration, "prefill", query, begin, first_prefill, begin == input));
                first_prefill = false;
            }
            for (std::size_t token = 1; token < output; ++token) {
                program.work.push_back(phase_work(target.configuration, "decode", 1, input + token, first_decode, true));
                first_decode = false;
            }
        }
    }
    return targets;
}

} // namespace markov::adapters::ktransformers
