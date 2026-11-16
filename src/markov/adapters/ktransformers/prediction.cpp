#include "markov/adapters/ktransformers/prediction.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <set>
#include <stdexcept>
#include <utility>

namespace markov::adapters::ktransformers {
namespace {

using namespace trace_graph;
using Json = nlohmann::json;

std::size_t positive(const Json & raw, const char * name) {
    const auto & value = raw.at(name);
    if (!value.is_number_integer() || value.get<std::int64_t>() <= 0) throw std::runtime_error(std::string(name) + " must be a positive integer");
    return value.get<std::size_t>();
}

} // namespace

PreparedPrediction prepare_prediction(const Json & raw, source::ProfileManifest manifest) {
    const auto deployment = raw.at("deployment");
    if (deployment.at("framework") != "ktransformers" || deployment.at("runtime") != "ascend" || deployment.at("dtype") != "bfloat16"
        || positive(deployment, "tensor_parallel") != 1)
        throw std::runtime_error("KT prediction supports the single-NPU BF16 DeepSeek deployment");
    if (raw.at("kernel_cache_initial_state") != "empty")
        throw std::runtime_error("KT currently preserves the base startup history; only kernel_cache_initial_state=empty is supported");
    std::ifstream stream(manifest.configuration_path.value());
    const auto captured = Json::parse(stream);
    if (captured.at("deployment") != deployment) throw std::runtime_error("KT base belongs to another deployment");
    const auto & model = captured.at("model_configuration");
    const auto & server = captured.at("server").at("arguments");
    if (model.at("model_type") != "deepseek_v4" || server.at("kt_method") != "LLAMAFILE" || server.at("quantization") != "compressed-tensors"
        || !server.at("disable_cuda_graph").get<bool>() || !server.at("disable_overlap_schedule").get<bool>() || !server.at("disable_radix_cache").get<bool>()
        || server.value("kt_max_deferred_experts_per_token", 0) != 0 || server.value("kt_expert_placement_strategy", "prefix") != "prefix")
        throw std::runtime_error("KT rules require eager, serial DeepSeek-V4 with prefix placement and no deferred experts");
    const auto & workload = raw.at("workload");
    const auto & actual = captured.at("workload");
    if (workload.at("requests").size() != actual.at("requests").size() || workload.at("formal_begin_index") != actual.at("formal_begin_index")
        || workload.value("drain_before_formal", false))
        throw std::runtime_error("KT prediction needs the same serial workload and formal boundary as its base");
    for (std::size_t id = 0; id < workload.at("requests").size(); ++id) {
        const auto & request = workload.at("requests")[id];
        const auto & observed = actual.at("requests")[id];
        if ((!request.at("input_ids").is_null() && request.at("input_ids") != observed.at("input_ids"))
            || (request.at("input_ids").is_null() && request.at("prompt") != observed.at("prompt"))
            || request.at("max_new_tokens") != observed.at("max_new_tokens") || !request.at("ignore_eos").get<bool>()
            || request.at("return_logprob").get<bool>())
            throw std::runtime_error("KT input tokens or fixed output count differ from the base workload");
    }
    std::map<std::size_t, SelectedCosts> costs;
    std::map<std::size_t, std::filesystem::path> paths;
    if (raw.at("models").size() != 1 || !raw.at("models").contains("1")) throw std::runtime_error("KT costs require one TP1 model selection");
    auto selected = read_cost_models(raw.at("models").at("1").get<std::string>());
    if (selected.deployment != deployment) throw std::runtime_error("KT independent costs belong to another deployment");
    paths.emplace(1, raw.at("models").at("1").get<std::string>());
    costs.emplace(1, std::move(selected));
    std::vector<PredictionTarget> targets;
    const std::set<std::string> variable{ "kt_cpuinfer", "kt_threadpool_count", "kt_num_gpu_experts" };
    for (const auto & item : raw.at("targets")) {
        if (item.at("deployment") != deployment) throw std::runtime_error("KT targets currently keep TP1 and the same model deployment");
        auto configuration = captured.at("configuration");
        for (const auto & [key, value] : item.at("configuration").items()) {
            if (!variable.contains(key) && (!configuration.contains(key) || configuration.at(key) != value))
                throw std::runtime_error("KT target changes an unsupported execution setting: " + key);
            configuration[key] = value;
        }
        const auto threads = positive(configuration, "kt_cpuinfer");
        const auto pools = positive(configuration, "kt_threadpool_count");
        const auto resident = positive(configuration, "kt_num_gpu_experts");
        const auto expert_width = positive(model, "moe_intermediate_size");
        if (threads < pools || expert_width % pools || expert_width % 256 || pools > expert_width / 256 || resident >= positive(model, "n_routed_experts"))
            throw std::runtime_error("KT CPU pools or expert partition are not valid for the native backend geometry");
        if (captured.contains("resource_envelope")) {
            const auto & resources = captured.at("resource_envelope");
            if (threads > resources.at("cpu_ids").size() || pools > resources.at("numa_nodes").get<std::size_t>())
                throw std::runtime_error("KT target exceeds the CPU/NUMA resources observed in the source deployment");
        }
        targets.push_back({ item.at("name").get<std::string>(), std::move(configuration), threads, pools, resident });
    }
    auto source = read_source(std::move(manifest), captured);
    if (source.source.context.observations().requests.size() != workload.at("requests").size())
        throw std::runtime_error("KT client report does not match the requested workload");
    for (std::size_t id = 0; id < workload.at("requests").size(); ++id) {
        const auto & request = source.source.context.observations().requests[id];
        if (request.identity != "request_" + std::to_string(id) || request.formal != (id >= workload.at("formal_begin_index").get<std::size_t>()))
            throw std::runtime_error("KT request identities or formal boundary do not match the workload");
    }
    for (const auto & batch : source.batches) {
        if (batch.layers.size() != positive(model, "num_hidden_layers")) throw std::runtime_error("KT source does not cover every model layer");
        for (const auto & layer : batch.layers) {
            if (layer.layer >= positive(model, "num_hidden_layers")) throw std::runtime_error("KT observed layer lies outside the model geometry");
            if (layer.expert_counts.size() != positive(model, "n_routed_experts") || layer.work.features.at("cpu_threads") != positive(server, "kt_cpuinfer")
                || layer.work.features.at("numa_pools") != positive(server, "kt_threadpool_count")
                || layer.work.features.at("resident_experts") != positive(server, "kt_num_gpu_experts"))
                throw std::runtime_error("KT observed resources disagree with the captured native invocation");
        }
    }
    return { std::move(source.source),
             std::move(source.batches),
             std::move(source.request_tails),
             deployment,
             std::move(costs),
             std::move(paths),
             std::move(targets),
             read_node_scale(raw.value("node_scale", Json::array())),
             raw.value("record_graph", false),
             captured.value("resource_envelope", Json::object()) };
}

void Program::start(planning::ProgramRuntime & runtime) {
    planning::PlannedMutation mutation{ 0, {}, {} };
    const auto & source = input_.source.context.observations();
    const auto append = [&](planning::PlannedOperation operation) {
        const auto id = runtime.next_node() + mutation.operations.size();
        if (!mutation.operations.empty()) mutation.dependencies.push_back({ id - 1, id });
        mutation.operations.push_back(std::move(operation));
        return id;
    };
    const auto observed_cost = [&](std::size_t index, std::size_t request, std::size_t iteration, const char * port) {
        planning::PlannedOperation operation{ 0, std::cref(source.cost_observations[index].work) };
        operation.source_cost = index;
        operation.position = planning::OperationPosition{ request, iteration, 0, port };
        return append(std::move(operation));
    };
    requests_.resize(source.requests.size());
    std::size_t batch_index = 0;
    for (std::size_t id = 0; id < source.requests.size(); ++id) {
        if (id)
            append({
                0,
                planning::SourceClientDelay{ id, planning::ClientInterval::BetweenRequests }
            });
        requests_[id].start = append({ 0, planning::Boundary{} });
        append({
            0,
            planning::SourceClientDelay{ id, planning::ClientInterval::Ingress }
        });
        while (batch_index < input_.batches.size() && input_.batches[batch_index].request == source.requests[id].identity) {
            const auto & batch = input_.batches[batch_index++];
            observed_cost(batch.frontend_cost, id, batch.iteration, "frontend");
            observed_cost(batch.residual_cost, id, batch.iteration, "non_moe");
            for (const auto & layer : batch.layers) {
                const auto phase = layer.work.kind.operation.substr(4);
                const auto query = static_cast<std::size_t>(layer.work.features.at("query_tokens"));
                work_.push_back(moe_work(phase, layer.expert_counts, query, target_.resident_experts, target_.cpu_threads, target_.numa_pools));
                planning::PlannedOperation operation{ 0, std::cref(work_.back()) };
                operation.source_cost = layer.source_cost;
                operation.position = planning::OperationPosition{ id, batch.iteration, 0, "moe" };
                append(std::move(operation));
            }
        }
        observed_cost(input_.request_tails[id], id, 0, "frontend_tail");
        append({
            0,
            planning::SourceClientDelay{ id, planning::ClientInterval::Egress }
        });
        planning::PlannedOperation response{ 0, planning::Boundary{} };
        response.completion = simulation::Action{ 0, id };
        requests_[id].response = append(std::move(response));
    }
    runtime.submit(std::move(mutation));
}

void Program::on_event(planning::ProgramRuntime &, const simulation::Event & event) { requests_.at(event.action.subject).completed = true; }

bool Program::complete() const {
    return std::ranges::all_of(requests_, [](const RequestResult & request) { return request.completed; });
}

} // namespace markov::adapters::ktransformers
